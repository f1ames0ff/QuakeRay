#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "alias_pose_jobs.h"
#include "tasks.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(condition, ...)                          \
	do                                                 \
	{                                                  \
		g_checks++;                                    \
		if (!(condition))                              \
		{                                              \
			g_failures++;                              \
			printf ("FAIL: ");                         \
			printf (__VA_ARGS__);                      \
			printf ("\n");                             \
		}                                              \
	} while (0)

static int g_alloc_calls = 0;
static int g_alloc_worker_calls = 0;
static int g_free_worker_calls = 0;

void *Mem_Alloc (const size_t size)
{
	void *block;

	g_alloc_calls++;

	if (Tasks_IsWorker ())
		g_alloc_worker_calls++;

	block = calloc (1, size);
	assert (block != NULL);
	return block;
}

void Mem_Free (const void *ptr)
{
	if (Tasks_IsWorker ())
		g_free_worker_calls++;

	free ((void *)ptr);
}

double Sys_DoubleTime (void)
{
	const double counter = (double)SDL_GetPerformanceCounter ();
	const double frequency = (double)SDL_GetPerformanceFrequency ();

	return counter / frequency;
}

static uint32_t g_rng = 0x2545F491u;

static uint32_t XorShift32 (void)
{
	uint32_t x = g_rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_rng = x;
	return x;
}

static void FillRandomVertices (QrVertex *vertices, int count)
{
	for (int i = 0; i < count; i++)
	{
		uint32_t *word = (uint32_t *)&vertices[i];

		for (size_t w = 0; w < sizeof (QrVertex) / sizeof (uint32_t); w++)
			word[w] = XorShift32 ();
	}
}

static QrVertex *AllocateVertices (int count)
{
	QrVertex *vertices = (QrVertex *)malloc ((size_t)count * sizeof (QrVertex));

	assert (vertices != NULL);
	return vertices;
}

static float TestLerp (float a, float b, float t)
{
	const float delta = b - a;

	return a + delta * t;
}

static void TestKernel (const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst, int first, int count,
                        float blend, int cluster)
{
	for (int i = first; i < first + count; i++)
	{
		dst[i] = pose1[i];
		dst[i].position[0] = TestLerp (pose1[i].position[0], pose2[i].position[0], blend);
		dst[i].position[1] = TestLerp (pose1[i].position[1], pose2[i].position[1], blend);
		dst[i].position[2] = TestLerp (pose1[i].position[2], pose2[i].position[2], blend);

		if (cluster > 0)
			dst[i].cluster = (uint32_t)cluster;
	}
}

static int   g_kernel_calls;
static int   g_kernel_on_worker;
static int   g_vertex_hits[16384];

static void RecordingKernel (const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst, int first, int count,
                             float blend, int cluster)
{
	g_kernel_calls++;

	if (Tasks_IsWorker ())
		g_kernel_on_worker = 1;

	for (int i = first; i < first + count; i++)
	{
		g_vertex_hits[i]++;
		dst[i] = pose1[i];
		dst[i].position[0] = (float)i + 1000.0f;
	}
}

static qboolean ExpectedBorrow (float blend, int cluster)
{
	return blend < FLT_EPSILON && cluster <= 0;
}

static void TestBorrowBoundaries (void)
{
	static const float blends[] = {0.0f, -0.0f, 0.25f, 1.0f, FLT_EPSILON, NAN, INFINITY};
	static const int   clusters[] = {-1, 0, 1, 2147483647};
	const int          count = 1024;
	QrVertex          *pose1 = AllocateVertices (count);
	QrVertex          *pose2 = AllocateVertices (count);
	QrVertex          *reference = AllocateVertices (count);

	FillRandomVertices (pose1, count);
	FillRandomVertices (pose2, count);

	for (size_t b = 0; b < sizeof (blends) / sizeof (blends[0]); b++)
	{
		for (size_t c = 0; c < sizeof (clusters) / sizeof (clusters[0]); c++)
		{
			const float    blend = blends[b];
			const int      cluster = clusters[c];
			const qboolean borrow = ExpectedBorrow (blend, cluster);
			alias_pose_result_t result;

			AliasPoseJobs_Begin (false);
			CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, blend, cluster, TestKernel, &result),
			       "prepare blend=%g cluster=%d", (double)blend, cluster);

			if (borrow)
			{
				CHECK (result.owner == ALIAS_POSE_OWNER_BORROWED, "borrow owner blend=%g cluster=%d", (double)blend, cluster);
				CHECK (result.vertices == pose1, "borrow pointer blend=%g cluster=%d", (double)blend, cluster);
				CHECK (result.slot == -1, "borrow slot blend=%g cluster=%d", (double)blend, cluster);
			}
			else
			{
				CHECK (result.owner == ALIAS_POSE_OWNER_SLOT, "owned owner blend=%g cluster=%d", (double)blend, cluster);
				CHECK (result.vertices != pose1 && result.vertices != pose2, "owned pointer blend=%g cluster=%d",
				       (double)blend, cluster);
			}

			AliasPoseJobs_Join ();

			if (!borrow)
			{
				memcpy (reference, pose1, (size_t)count * sizeof (QrVertex));
				TestKernel (pose1, pose2, reference, 0, count, blend, cluster);
				CHECK (memcmp (reference, result.vertices, (size_t)count * sizeof (QrVertex)) == 0,
				       "bytes match blend=%g cluster=%d", (double)blend, cluster);
			}

			AliasPoseJobs_Release (&result);
		}
	}

	free (reference);
	free (pose2);
	free (pose1);
}

static void TestSerialParallelEquivalence (void)
{
	static const int   counts[] = {1, 255, 256, 257, 4097, 10000};
	static const float blends[] = {0.25f, 1.0f};
	static const int   clusters[] = {-1, 0, 7};
	const int          job_count = (int)(sizeof (counts) / sizeof (counts[0]));
	QrVertex          *pose1[6];
	QrVertex          *pose2[6];
	QrVertex          *reference[6];
	QrVertex          *serial_copy[6];
	alias_pose_result_t results[6];

	for (int j = 0; j < job_count; j++)
	{
		pose1[j] = AllocateVertices (counts[j]);
		pose2[j] = AllocateVertices (counts[j]);
		reference[j] = AllocateVertices (counts[j]);
		serial_copy[j] = AllocateVertices (counts[j]);

		FillRandomVertices (pose1[j], counts[j]);
		FillRandomVertices (pose2[j], counts[j]);
	}

	for (size_t b = 0; b < sizeof (blends) / sizeof (blends[0]); b++)
	{
		for (size_t c = 0; c < sizeof (clusters) / sizeof (clusters[0]); c++)
		{
			const float blend = blends[b];
			const int   cluster = clusters[c];

			for (int j = 0; j < job_count; j++)
			{
				memcpy (reference[j], pose1[j], (size_t)counts[j] * sizeof (QrVertex));
				TestKernel (pose1[j], pose2[j], reference[j], 0, counts[j], blend, cluster);
			}

			AliasPoseJobs_Begin (false);

			for (int j = 0; j < job_count; j++)
			{
				CHECK (AliasPoseJobs_Prepare (pose1[j], pose2[j], counts[j], blend, cluster, TestKernel, &results[j]),
				       "serial prepare job %d", j);
			}

			AliasPoseJobs_Join ();

			CHECK (AliasPoseJobs_Stats ()->worker_ms == 0.0, "serial worker_ms stays zero");

			for (int j = 0; j < job_count; j++)
			{
				const size_t bytes = (size_t)counts[j] * sizeof (QrVertex);

				CHECK (memcmp (reference[j], results[j].vertices, bytes) == 0, "serial oracle job %d", j);
				memcpy (serial_copy[j], results[j].vertices, bytes);
			}

			for (int j = 0; j < job_count; j++)
				AliasPoseJobs_Release (&results[j]);

			AliasPoseJobs_Begin (true);

			for (int j = 0; j < job_count; j++)
			{
				CHECK (AliasPoseJobs_Prepare (pose1[j], pose2[j], counts[j], blend, cluster, TestKernel, &results[j]),
				       "parallel prepare job %d", j);
			}

			AliasPoseJobs_Join ();

			for (int j = 0; j < job_count; j++)
			{
				const size_t bytes = (size_t)counts[j] * sizeof (QrVertex);

				CHECK (memcmp (serial_copy[j], results[j].vertices, bytes) == 0, "parallel equals serial job %d", j);
				CHECK (memcmp (reference[j], results[j].vertices, bytes) == 0, "parallel oracle job %d", j);
			}

			for (int j = 0; j < job_count; j++)
				AliasPoseJobs_Release (&results[j]);
		}
	}

	for (int j = 0; j < job_count; j++)
	{
		free (serial_copy[j]);
		free (reference[j]);
		free (pose2[j]);
		free (pose1[j]);
	}
}

static void TestPartitionCoverage (void)
{
	const int count = 10000;
	QrVertex *pose1 = AllocateVertices (count);
	QrVertex *pose2 = AllocateVertices (count);
	alias_pose_result_t result;

	FillRandomVertices (pose1, count);
	FillRandomVertices (pose2, count);
	memset (g_vertex_hits, 0, sizeof (g_vertex_hits));

	AliasPoseJobs_Begin (true);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 3, RecordingKernel, &result), "coverage prepare");
	AliasPoseJobs_Join ();

	for (int i = 0; i < count; i++)
	{
		CHECK (g_vertex_hits[i] == 1, "vertex %d covered once (hits %d)", i, g_vertex_hits[i]);
		CHECK (result.vertices[i].position[0] == (float)i + 1000.0f, "vertex %d holds its own value", i);
	}

	CHECK (AliasPoseJobs_Stats ()->chunks >= 1 && AliasPoseJobs_Stats ()->chunks <= 32, "chunk count bounded");

	AliasPoseJobs_Release (&result);
	free (pose2);
	free (pose1);
}

static void TestLifetimeAndSlots (void)
{
	const int count = 4000;
	QrVertex *pose1 = AllocateVertices (count);
	QrVertex *pose2 = AllocateVertices (count);
	QrVertex *snapshot = AllocateVertices (count);
	QrVertex *reference = AllocateVertices (count);
	alias_pose_result_t a;
	alias_pose_result_t b;
	alias_pose_result_t c;
	alias_pose_result_t fill[8];

	FillRandomVertices (pose1, count);
	FillRandomVertices (pose2, count);

	AliasPoseJobs_Begin (false);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 2, TestKernel, &a), "prepare a");
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 2, TestKernel, &b), "prepare b");
	CHECK (a.slot != b.slot, "distinct slots for concurrent results");
	AliasPoseJobs_Join ();
	memcpy (snapshot, a.vertices, (size_t)count * sizeof (QrVertex));

	AliasPoseJobs_Begin (false);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 2, TestKernel, &c), "prepare c");
	AliasPoseJobs_Join ();
	CHECK (memcmp (snapshot, a.vertices, (size_t)count * sizeof (QrVertex)) == 0, "lease a stays intact while c is prepared");

	AliasPoseJobs_Release (&a);
	CHECK (a.owner == ALIAS_POSE_OWNER_BORROWED && a.vertices == NULL && a.slot == -1, "release resets the result");
	AliasPoseJobs_Release (&a);
	AliasPoseJobs_Release (&b);
	AliasPoseJobs_Release (&c);

	AliasPoseJobs_Begin (false);

	for (int i = 0; i < 8; i++)
		CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 2, TestKernel, &fill[i]), "fill slot %d", i);

	{
		const int misses_before = AliasPoseJobs_Stats ()->slot_misses;
		alias_pose_result_t overflow;

		CHECK (!AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 2, TestKernel, &overflow), "ninth lease is refused");
		CHECK (AliasPoseJobs_Stats ()->slot_misses == misses_before + 1, "slot miss counted");
	}

	AliasPoseJobs_Join ();
	memcpy (reference, pose1, (size_t)count * sizeof (QrVertex));
	TestKernel (pose1, pose2, reference, 0, count, 0.5f, 2);

	for (int i = 0; i < 8; i++)
	{
		CHECK (memcmp (reference, fill[i].vertices, (size_t)count * sizeof (QrVertex)) == 0,
		       "full pool keeps lease %d correct", i);
		AliasPoseJobs_Release (&fill[i]);
	}

	free (reference);
	free (snapshot);
	free (pose2);
	free (pose1);
}

static void TestMisuse (void)
{
	alias_pose_result_t result;
	alias_pose_result_t sentinel;
	QrVertex *pose1 = AllocateVertices (16);
	QrVertex *pose2 = AllocateVertices (16);

	FillRandomVertices (pose1, 16);
	FillRandomVertices (pose2, 16);

	AliasPoseJobs_Begin (false);

	sentinel = (alias_pose_result_t){pose1, 999, 7, ALIAS_POSE_OWNER_BORROWED};
	CHECK (!AliasPoseJobs_Prepare (NULL, pose2, 16, 0.5f, 0, TestKernel, &sentinel), "null pose rejected");
	CHECK (sentinel.vertices == pose1 && sentinel.slot == 7, "result untouched on null pose");

	sentinel = (alias_pose_result_t){pose1, 999, 7, ALIAS_POSE_OWNER_BORROWED};
	CHECK (!AliasPoseJobs_Prepare (pose1, pose2, -1, 0.5f, 0, TestKernel, &sentinel), "negative count rejected");
	CHECK (sentinel.vertices == pose1 && sentinel.slot == 7, "result untouched on negative count");

	sentinel = (alias_pose_result_t){pose1, 999, 7, ALIAS_POSE_OWNER_BORROWED};
	CHECK (!AliasPoseJobs_Prepare (pose1, pose2, 1 << 30, 0.5f, 0, TestKernel, &sentinel),
	       "too many vertices rejected");
	CHECK (sentinel.vertices == pose1 && sentinel.slot == 7, "result untouched on overflow count");

	CHECK (!AliasPoseJobs_Prepare (pose1, pose2, 16, 0.5f, 0, NULL, &sentinel), "null kernel rejected");

	CHECK (AliasPoseJobs_Prepare (pose1, pose2, 0, 0.5f, 0, TestKernel, &result), "zero count accepted");
	CHECK (result.owner == ALIAS_POSE_OWNER_BORROWED && result.vertices == pose1, "zero count borrows pose1");

	AliasPoseJobs_Join ();
	AliasPoseJobs_Release (&result);
	AliasPoseJobs_Release (NULL);

	free (pose2);
	free (pose1);
}

static void TestWorkerDiscipline (void)
{
	const int count = 10000;
	QrVertex *pose1 = AllocateVertices (count);
	QrVertex *pose2 = AllocateVertices (count);
	alias_pose_result_t result;

	FillRandomVertices (pose1, count);
	FillRandomVertices (pose2, count);

	g_kernel_calls = 0;
	g_kernel_on_worker = 0;
	g_alloc_worker_calls = 0;
	g_free_worker_calls = 0;

	AliasPoseJobs_Begin (true);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 5, RecordingKernel, &result), "discipline prepare");
	AliasPoseJobs_Join ();

	CHECK (g_alloc_worker_calls == 0, "no allocation from a worker (saw %d)", g_alloc_worker_calls);
	CHECK (g_free_worker_calls == 0, "no free from a worker (saw %d)", g_free_worker_calls);
	CHECK (g_kernel_calls >= 1, "kernel ran");

	if (Tasks_NumWorkers () > 1 && AliasPoseJobs_Stats ()->chunks > 1)
		CHECK (g_kernel_on_worker == 1, "at least one chunk ran on a worker");
	else
		printf ("note: parallel path not exercised (workers=%d, chunks=%d)\n", Tasks_NumWorkers (),
		        AliasPoseJobs_Stats ()->chunks);

	AliasPoseJobs_Release (&result);
	free (pose2);
	free (pose1);
}

static void TestTimingSeparation (void)
{
	const int count = 4096;
	QrVertex *pose1 = AllocateVertices (count);
	QrVertex *pose2 = AllocateVertices (count);
	alias_pose_result_t result;

	FillRandomVertices (pose1, count);
	FillRandomVertices (pose2, count);

	AliasPoseJobs_Begin (true);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 1, TestKernel, &result), "timing prepare");
	AliasPoseJobs_Join ();

	{
		const alias_pose_stats_t *stats = AliasPoseJobs_Stats ();

		CHECK (isfinite (stats->phase_ms) && stats->phase_ms >= 0.0, "phase_ms finite");
		CHECK (isfinite (stats->worker_ms) && stats->worker_ms >= 0.0, "worker_ms finite");
		CHECK (stats->jobs == 1 && stats->chunks >= 1, "job and chunk counters reported");
	}

	AliasPoseJobs_Release (&result);

	AliasPoseJobs_Begin (false);
	CHECK (AliasPoseJobs_Prepare (pose1, pose2, count, 0.5f, 1, TestKernel, &result), "timing serial prepare");
	AliasPoseJobs_Join ();
	CHECK (AliasPoseJobs_Stats ()->worker_ms == 0.0, "serial run reports no worker time");
	AliasPoseJobs_Release (&result);

	free (pose2);
	free (pose1);
}

int main (void)
{
	Tasks_Init ();

	printf ("alias_pose_jobs_tests: workers=%d\n", Tasks_NumWorkers ());

	TestBorrowBoundaries ();
	TestSerialParallelEquivalence ();
	TestPartitionCoverage ();
	TestLifetimeAndSlots ();
	TestMisuse ();
	TestWorkerDiscipline ();
	TestTimingSeparation ();

	AliasPoseJobs_Shutdown ();

	printf ("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
