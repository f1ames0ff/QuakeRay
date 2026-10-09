#include "quakedef.h"
#include "tasks.h"
#include "mem.h"
#include "alias_pose_jobs.h"

#define ALIAS_POSE_SLOT_COUNT     8
#define ALIAS_POSE_CHUNK_VERTS    256
#define ALIAS_POSE_MAX_CHUNKS_JOB 32
#define ALIAS_POSE_MAX_JOBS       64
#define ALIAS_POSE_MAX_CHUNKS     (ALIAS_POSE_MAX_JOBS * ALIAS_POSE_MAX_CHUNKS_JOB)
#define ALIAS_POSE_MAX_VERTS      262144
#define ALIAS_POSE_GROW_STEP      4096

typedef struct
{
	QrVertex *vertices;
	int       capacity;
	qboolean  leased;
} alias_pose_slot_t;

typedef struct
{
	const QrVertex     *pose1;
	const QrVertex     *pose2;
	QrVertex           *dst;
	int                 vertex_count;
	float               blend;
	int                 cluster;
	alias_pose_kernel_t kernel;
} alias_pose_job_t;

typedef struct
{
	int    job;
	int    first;
	int    count;
	double worker_ms;
} alias_pose_chunk_t;

static alias_pose_slot_t  alias_pose_slots[ALIAS_POSE_SLOT_COUNT];
static alias_pose_job_t   alias_pose_job_list[ALIAS_POSE_MAX_JOBS];
static alias_pose_chunk_t alias_pose_chunk_list[ALIAS_POSE_MAX_CHUNKS];
static int                alias_pose_job_count;
static int                alias_pose_chunk_count;
static qboolean           alias_pose_parallel;
static qboolean           alias_pose_active;
static alias_pose_stats_t alias_pose_stats;

static int AliasPoseJobs_Step (int vertex_count)
{
	const int step = ALIAS_POSE_GROW_STEP;

	return ((vertex_count + step - 1) / step) * step;
}

static void AliasPoseJobs_Chunk (int index, void *unused)
{
	const alias_pose_job_t *job = &alias_pose_job_list[alias_pose_chunk_list[index].job];
	const int               first = alias_pose_chunk_list[index].first;
	const int               count = alias_pose_chunk_list[index].count;
	const double            start = Sys_DoubleTime ();

	job->kernel (job->pose1, job->pose2, job->dst, first, count, job->blend, job->cluster);
	alias_pose_chunk_list[index].worker_ms = (Sys_DoubleTime () - start) * 1000.0;
}

static int AliasPoseJobs_AcquireSlot (int vertex_count)
{
	for (int i = 0; i < ALIAS_POSE_SLOT_COUNT; i++)
	{
		if (!alias_pose_slots[i].leased && alias_pose_slots[i].capacity >= vertex_count)
			return i;
	}

	for (int i = 0; i < ALIAS_POSE_SLOT_COUNT; i++)
	{
		const int capacity = AliasPoseJobs_Step (vertex_count);
		QrVertex *block;

		if (alias_pose_slots[i].leased)
			continue;

		block = (QrVertex *)Mem_Alloc ((size_t)capacity * sizeof (QrVertex));

		if (block == NULL)
			return -1;

		if (alias_pose_slots[i].vertices != NULL)
			Mem_Free (alias_pose_slots[i].vertices);

		alias_pose_slots[i].vertices = block;
		alias_pose_slots[i].capacity = capacity;
		return i;
	}

	return -1;
}

void AliasPoseJobs_Begin (qboolean parallel)
{
	assert (!Tasks_IsWorker ());
	assert (!alias_pose_active);

	alias_pose_parallel = parallel && Tasks_NumWorkers () > 1;
	alias_pose_job_count = 0;
	alias_pose_chunk_count = 0;
	alias_pose_active = true;
	memset (&alias_pose_stats, 0, sizeof (alias_pose_stats));
}

qboolean AliasPoseJobs_Prepare (const QrVertex *pose1, const QrVertex *pose2, int vertex_count, float blend,
                                int cluster, alias_pose_kernel_t kernel, alias_pose_result_t *result)
{
	int slot;
	int num_chunks;
	int span;
	int job_index;

	assert (!Tasks_IsWorker ());
	assert (alias_pose_active);

	if (pose1 == NULL || pose2 == NULL || kernel == NULL || result == NULL)
		return false;

	if (vertex_count < 0 || vertex_count > ALIAS_POSE_MAX_VERTS)
		return false;

	if (vertex_count == 0 || (blend < FLT_EPSILON && cluster <= 0))
	{
		result->vertices = pose1;
		result->vertex_count = vertex_count;
		result->slot = -1;
		result->owner = ALIAS_POSE_OWNER_BORROWED;
		alias_pose_stats.borrowed++;
		return true;
	}

	slot = AliasPoseJobs_AcquireSlot (vertex_count);

	if (slot < 0)
	{
		alias_pose_stats.slot_misses++;
		return false;
	}

	num_chunks = (vertex_count + ALIAS_POSE_CHUNK_VERTS - 1) / ALIAS_POSE_CHUNK_VERTS;

	if (num_chunks > ALIAS_POSE_MAX_CHUNKS_JOB)
		num_chunks = ALIAS_POSE_MAX_CHUNKS_JOB;

	if (alias_pose_job_count >= ALIAS_POSE_MAX_JOBS || alias_pose_chunk_count + num_chunks > ALIAS_POSE_MAX_CHUNKS)
	{
		alias_pose_stats.slot_misses++;
		return false;
	}

	span = (vertex_count + num_chunks - 1) / num_chunks;
	job_index = alias_pose_job_count;

	alias_pose_job_list[job_index].pose1 = pose1;
	alias_pose_job_list[job_index].pose2 = pose2;
	alias_pose_job_list[job_index].dst = alias_pose_slots[slot].vertices;
	alias_pose_job_list[job_index].vertex_count = vertex_count;
	alias_pose_job_list[job_index].blend = blend;
	alias_pose_job_list[job_index].cluster = cluster;
	alias_pose_job_list[job_index].kernel = kernel;
	alias_pose_job_count++;

	for (int i = 0; i < num_chunks; i++)
	{
		int first = i * span;
		int count = span;

		if (first >= vertex_count)
			break;

		if (first + count > vertex_count)
			count = vertex_count - first;

		alias_pose_chunk_list[alias_pose_chunk_count].job = job_index;
		alias_pose_chunk_list[alias_pose_chunk_count].first = first;
		alias_pose_chunk_list[alias_pose_chunk_count].count = count;
		alias_pose_chunk_list[alias_pose_chunk_count].worker_ms = 0.0;
		alias_pose_chunk_count++;
	}

	alias_pose_slots[slot].leased = true;

	result->vertices = alias_pose_slots[slot].vertices;
	result->vertex_count = vertex_count;
	result->slot = slot;
	result->owner = ALIAS_POSE_OWNER_SLOT;
	return true;
}

void AliasPoseJobs_Join (void)
{
	double worker_ms = 0.0;

	assert (!Tasks_IsWorker ());
	assert (alias_pose_active);

	alias_pose_active = false;
	alias_pose_stats.jobs = alias_pose_job_count;
	alias_pose_stats.chunks = alias_pose_chunk_count;

	if (alias_pose_chunk_count == 0)
		return;

	if (alias_pose_parallel)
	{
		const double start = Sys_DoubleTime ();
		task_handle_t handle =
		    Task_AllocateAndAssignIndexedFunc (AliasPoseJobs_Chunk, (uint32_t)alias_pose_chunk_count, NULL, 0);

		Task_Submit (handle);

		while (!Task_Join (handle, SDL_MUTEX_MAXWAIT))
		{
		}

		alias_pose_stats.phase_ms = (Sys_DoubleTime () - start) * 1000.0;

		for (int i = 0; i < alias_pose_chunk_count; i++)
			worker_ms += alias_pose_chunk_list[i].worker_ms;

		alias_pose_stats.worker_ms = worker_ms;
	}
	else
	{
		const double start = Sys_DoubleTime ();

		for (int i = 0; i < alias_pose_chunk_count; i++)
			AliasPoseJobs_Chunk (i, NULL);

		alias_pose_stats.phase_ms = (Sys_DoubleTime () - start) * 1000.0;
		alias_pose_stats.worker_ms = 0.0;
	}
}

void AliasPoseJobs_Release (alias_pose_result_t *result)
{
	if (result == NULL)
		return;

	assert (!Tasks_IsWorker ());
	assert (!alias_pose_active);

	if (result->owner == ALIAS_POSE_OWNER_SLOT && result->slot >= 0 && result->slot < ALIAS_POSE_SLOT_COUNT)
		alias_pose_slots[result->slot].leased = false;

	result->vertices = NULL;
	result->vertex_count = 0;
	result->slot = -1;
	result->owner = ALIAS_POSE_OWNER_BORROWED;
}

const alias_pose_stats_t *AliasPoseJobs_Stats (void)
{
	return &alias_pose_stats;
}

void AliasPoseJobs_Shutdown (void)
{
	assert (!Tasks_IsWorker ());

	for (int i = 0; i < ALIAS_POSE_SLOT_COUNT; i++)
	{
		if (alias_pose_slots[i].vertices != NULL)
			Mem_Free (alias_pose_slots[i].vertices);

		alias_pose_slots[i].vertices = NULL;
		alias_pose_slots[i].capacity = 0;
		alias_pose_slots[i].leased = false;
	}
}
