#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alias_pose_jobs.h"

double RT_Prof_Begin (void);
void   RT_Prof_End (int slot, double start);

double RT_Prof_Begin (void)
{
	return 0.0;
}

void RT_Prof_End (int slot, double start)
{
	(void)slot;
	(void)start;
}

void *Mem_Alloc (const size_t size)
{
	void *block = calloc (1, size);

	assert (block != NULL);
	return block;
}

void Mem_Free (const void *ptr)
{
	free ((void *)ptr);
}

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(condition, ...)          \
	do                                 \
	{                                  \
		g_checks++;                    \
		if (!(condition))              \
		{                              \
			g_failures++;              \
			printf ("FAIL: ");         \
			printf (__VA_ARGS__);      \
			printf ("\n");             \
		}                              \
	} while (0)

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

static void TestKernelRef (const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst, int first, int count,
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

static void TestKernelEquivalence (void)
{
	static const int   counts[] = {0, 1, 2, 255, 256, 257, 1000};
	static const float blends[] = {0.0f, 0.25f, 1.0f};
	static const int   clusters[] = {0, 1, 7};
	const int          first = 8;
	const int          num_counts = (int)(sizeof (counts) / sizeof (counts[0]));
	const int          num_blends = (int)(sizeof (blends) / sizeof (blends[0]));
	const int          num_clusters = (int)(sizeof (clusters) / sizeof (clusters[0]));

	for (int c = 0; c < num_counts; c++)
	{
		const int count = counts[c];
		const int total = count + 2 * first;
		const size_t bytes = (size_t)total * sizeof (QrVertex);
		QrVertex    *pose1 = AllocateVertices (total);
		QrVertex    *pose2 = AllocateVertices (total);
		QrVertex    *dst = AllocateVertices (total);
		QrVertex    *expected = AllocateVertices (total);

		FillRandomVertices (pose1, total);
		FillRandomVertices (pose2, total);

		for (int b = 0; b < num_blends; b++)
		{
			for (int k = 0; k < num_clusters; k++)
			{
				FillRandomVertices (dst, total);
				memcpy (expected, dst, bytes);

				TestKernelRef (pose1, pose2, expected, first, count, blends[b], clusters[k]);
				AliasPoseJobs_Kernel (pose1, pose2, dst, first, count, blends[b], clusters[k]);

				CHECK (memcmp (dst, expected, bytes) == 0, "kernel count=%d blend=%g cluster=%d", count,
				       (double)blends[b], clusters[k]);
			}
		}

		free (expected);
		free (dst);
		free (pose2);
		free (pose1);
	}
}

static void TestNeedsCopyBoundaries (void)
{
	const float blends[] = {0.0f, -0.0f, nextafterf (FLT_EPSILON, 0.0f), FLT_EPSILON, 1.0f, NAN, -1.0f, -FLT_EPSILON};
	const int   clusters[] = {-1, 0, 1};

	for (size_t b = 0; b < sizeof (blends) / sizeof (blends[0]); b++)
	{
		for (size_t c = 0; c < sizeof (clusters) / sizeof (clusters[0]); c++)
		{
			const qboolean expected = !(blends[b] < FLT_EPSILON && clusters[c] <= 0);

			CHECK (AliasPoseJobs_NeedsCopy (blends[b], clusters[c]) == expected, "needs copy blend=%g cluster=%d",
			       (double)blends[b], clusters[c]);
		}
	}
}

#define TEST_LIFECYCLE_LIMIT 48
#define TEST_STRIDE          8
#define TEST_MAX_VERTICES    8

static QrVertex *g_pose_pool1;
static QrVertex *g_pose_pool2;
static int       g_provider_limit;

static qboolean StrideProvider (int index, alias_pose_request_t *out)
{
	if (index < 0 || index >= g_provider_limit)
		return false;

	out->pose1 = g_pose_pool1 + index * TEST_STRIDE;
	out->pose2 = g_pose_pool2 + index * TEST_STRIDE;
	out->vertex_count = 4 + (index % 5);
	out->blend = 0.25f + (float)(index % 7) * 0.125f;
	out->cluster = (index % 3) - 1;
	return true;
}

static void TestLifecycle (void)
{
	alias_pose_request_t request;
	const QrVertex      *prepared;
	static QrVertex      expected[TEST_MAX_VERTICES];
	const int            pool_size = TEST_LIFECYCLE_LIMIT * TEST_STRIDE;

	g_pose_pool1 = AllocateVertices (pool_size);
	g_pose_pool2 = AllocateVertices (pool_size);
	FillRandomVertices (g_pose_pool1, pool_size);
	FillRandomVertices (g_pose_pool2, pool_size);
	g_provider_limit = TEST_LIFECYCLE_LIMIT;

	AliasPoseJobs_Begin (TEST_LIFECYCLE_LIMIT, StrideProvider);

	{
		const int          unit_count = AliasPoseJobs_TaskLimit ();
		const unsigned int prepared_before = AliasPoseJobs_Stats ()->prepared;

		CHECK (unit_count >= 1, "task limit is at least one unit");

		for (int unit = 0; unit < unit_count; unit++)
			AliasPoseJobs_PrepareTask (unit, NULL);

		AliasPoseJobs_PrepareTask (unit_count, NULL);
		CHECK (AliasPoseJobs_Stats ()->prepared == prepared_before + (unsigned)TEST_LIFECYCLE_LIMIT,
		       "units beyond the limit prepare nothing");
	}

	CHECK (AliasPoseJobs_Stats ()->prepared == (unsigned)TEST_LIFECYCLE_LIMIT, "every index prepared once");
	CHECK (AliasPoseJobs_Stats ()->declined == 0, "no declines");
	CHECK (AliasPoseJobs_Stats ()->overflow == 0, "no overflow");

	for (int i = 0; i < TEST_LIFECYCLE_LIMIT; i++)
	{
		CHECK (StrideProvider (i, &request), "provider index %d", i);
		prepared = NULL;
		CHECK (AliasPoseJobs_Lookup (i, request.pose1, request.pose2, request.vertex_count, request.blend,
		                             request.cluster, &prepared),
		       "lookup hit %d", i);
		CHECK (prepared != NULL, "hit pointer %d", i);

		if (prepared != NULL)
		{
			memset (expected, 0, sizeof (expected));
			TestKernelRef (request.pose1, request.pose2, expected, 0, request.vertex_count, request.blend,
			               request.cluster);
			CHECK (memcmp (prepared, expected, (size_t)request.vertex_count * sizeof (QrVertex)) == 0,
			       "lookup content %d", i);
		}
	}

	CHECK (StrideProvider (5, &request), "provider index 5");
	CHECK (AliasPoseJobs_Lookup (5, request.pose1, request.pose2, request.vertex_count, request.blend,
	                             request.cluster, &prepared),
	       "exact lookup");
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1 + 1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "pose1 mismatch");
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1, request.pose2 + 1, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "pose2 mismatch");
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1, request.pose2, request.vertex_count + 1, request.blend,
	                              request.cluster, &prepared),
	       "count mismatch");
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1, request.pose2, request.vertex_count, request.blend + 0.5f,
	                              request.cluster, &prepared),
	       "blend mismatch");
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster + 1, &prepared),
	       "cluster mismatch");
	CHECK (!AliasPoseJobs_Lookup (-1, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "negative index");
	CHECK (!AliasPoseJobs_Lookup (TEST_LIFECYCLE_LIMIT, request.pose1, request.pose2, request.vertex_count,
	                              request.blend, request.cluster, &prepared),
	       "out of range index");

	AliasPoseJobs_Begin (TEST_LIFECYCLE_LIMIT, StrideProvider);
	CHECK (!AliasPoseJobs_Lookup (5, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "stale serial");

	free (g_pose_pool2);
	free (g_pose_pool1);
}

static QrVertex *g_big_pose1;
static QrVertex *g_big_pose2;
static int       g_big_count;

static qboolean BigProvider (int index, alias_pose_request_t *out)
{
	if (index != 0)
		return false;

	out->pose1 = g_big_pose1;
	out->pose2 = g_big_pose2;
	out->vertex_count = g_big_count;
	out->blend = 0.5f;
	out->cluster = 3;
	return true;
}

static void TestArenaOverflow (void)
{
	const int huge_count = 65536 + 1024;
	const int fitted_count = 70000;
	const QrVertex *prepared;
	QrVertex       *expected;

	g_big_pose1 = AllocateVertices (fitted_count);
	g_big_pose2 = AllocateVertices (fitted_count);
	FillRandomVertices (g_big_pose1, fitted_count);
	FillRandomVertices (g_big_pose2, fitted_count);

	g_big_count = huge_count;
	AliasPoseJobs_Begin (4, BigProvider);
	AliasPoseJobs_PrepareTask (0, NULL);
	CHECK (AliasPoseJobs_Stats ()->overflow == 1, "overflow counted");
	CHECK (AliasPoseJobs_Stats ()->prepared == 0, "overflow publishes nothing");

	prepared = NULL;
	CHECK (!AliasPoseJobs_Lookup (0, g_big_pose1, g_big_pose2, huge_count, 0.5f, 3, &prepared), "overflow fallback");
	CHECK (prepared == NULL, "overflow leaves the output untouched");

	g_big_count = fitted_count;
	AliasPoseJobs_Begin (4, BigProvider);
	AliasPoseJobs_PrepareTask (0, NULL);
	CHECK (AliasPoseJobs_Stats ()->prepared == 1, "arena grew for the next frame");

	prepared = NULL;
	CHECK (AliasPoseJobs_Lookup (0, g_big_pose1, g_big_pose2, fitted_count, 0.5f, 3, &prepared), "grown lookup");
	CHECK (prepared != NULL, "grown pointer");

	if (prepared != NULL)
	{
		expected = AllocateVertices (fitted_count);
		TestKernelRef (g_big_pose1, g_big_pose2, expected, 0, fitted_count, 0.5f, 3);
		CHECK (memcmp (prepared, expected, (size_t)fitted_count * sizeof (QrVertex)) == 0, "grown content");
		free (expected);
	}

	free (g_big_pose2);
	free (g_big_pose1);
}

static void TestDisabledMode (void)
{
	alias_pose_request_t request;
	const QrVertex      *prepared = NULL;
	unsigned int         prepared_before;
	QrVertex             pose1[TEST_MAX_VERTICES];
	QrVertex             pose2[TEST_MAX_VERTICES];

	request.pose1 = pose1;
	request.pose2 = pose2;
	request.vertex_count = TEST_MAX_VERTICES;
	request.blend = 0.5f;
	request.cluster = 2;

	AliasPoseJobs_Begin (0, StrideProvider);
	CHECK (!AliasPoseJobs_Lookup (0, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "zero capacity disables");

	AliasPoseJobs_Begin (16, NULL);
	CHECK (!AliasPoseJobs_Lookup (0, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "null provider disables");

	prepared_before = AliasPoseJobs_Stats ()->prepared;
	AliasPoseJobs_PrepareTask (0, NULL);
	CHECK (AliasPoseJobs_Stats ()->prepared == prepared_before, "disabled prepare is a no-op");

	AliasPoseJobs_Disable ();
	CHECK (!AliasPoseJobs_Lookup (0, request.pose1, request.pose2, request.vertex_count, request.blend,
	                              request.cluster, &prepared),
	       "explicit disable");
}

int main (void)
{
	printf ("alias_pose_jobs_tests\n");

	TestKernelEquivalence ();
	TestNeedsCopyBoundaries ();
	TestLifecycle ();
	TestArenaOverflow ();
	TestDisabledMode ();

	AliasPoseJobs_Shutdown ();

	printf ("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
