#include "quakedef.h"
#include "tasks.h"
#include "mem.h"
#include "atomics.h"
#include "alias_pose_jobs.h"

#define ALIAS_POSE_ARENA_INITIAL_VERTS 65536
#define ALIAS_POSE_ARENA_MAX_VERTS     524288

typedef struct
{
	const QrVertex *pose1;
	const QrVertex *pose2;
	const QrVertex *vertices;
	int             vertex_count;
	float           blend;
	int             cluster;
	int             serial;
} alias_pose_entry_t;

static alias_pose_entry_t   *alias_pose_entries;
static int                   alias_pose_entry_capacity;
static QrVertex             *alias_pose_arena;
static int                   alias_pose_arena_capacity;
static atomic_uint32_t       alias_pose_arena_bump;
static atomic_uint32_t       alias_pose_stat_prepared;
static atomic_uint32_t       alias_pose_stat_declined;
static atomic_uint32_t       alias_pose_stat_overflow;
static atomic_uint32_t       alias_pose_stat_hits;
static atomic_uint32_t       alias_pose_stat_misses;
static alias_pose_stats_t    alias_pose_stats;
static int                   alias_pose_serial;
static qboolean              alias_pose_enabled;
static alias_pose_provider_t alias_pose_provider;

void AliasPoseJobs_Kernel (const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst, int first, int count,
                           float blend, int cluster)
{
	for (int i = first; i < first + count; i++)
	{
		dst[i] = pose1[i];

		for (int j = 0; j < 3; j++)
			dst[i].position[j] = pose1[i].position[j] + (pose2[i].position[j] - pose1[i].position[j]) * blend;

		if (cluster > 0)
			dst[i].cluster = (uint32_t)cluster;
	}
}

qboolean AliasPoseJobs_NeedsCopy (float blend, int cluster)
{
	return !(blend < FLT_EPSILON && cluster <= 0);
}

void AliasPoseJobs_Begin (int visedict_capacity, alias_pose_provider_t provider)
{
	if (visedict_capacity <= 0 || provider == NULL)
	{
		AliasPoseJobs_Disable ();
		return;
	}

	alias_pose_serial++;
	Atomic_StoreUInt32 (&alias_pose_stat_prepared, 0);
	Atomic_StoreUInt32 (&alias_pose_stat_declined, 0);
	Atomic_StoreUInt32 (&alias_pose_stat_overflow, 0);
	Atomic_StoreUInt32 (&alias_pose_stat_hits, 0);
	Atomic_StoreUInt32 (&alias_pose_stat_misses, 0);

	if (visedict_capacity > alias_pose_entry_capacity)
	{
		alias_pose_entry_t *entries = (alias_pose_entry_t *)Mem_Alloc (sizeof (alias_pose_entry_t) * visedict_capacity);

		memset (entries, 0, sizeof (alias_pose_entry_t) * visedict_capacity);
		Mem_Free (alias_pose_entries);
		alias_pose_entries = entries;
		alias_pose_entry_capacity = visedict_capacity;
	}

	if (alias_pose_arena == NULL)
	{
		alias_pose_arena_capacity = ALIAS_POSE_ARENA_INITIAL_VERTS;
		alias_pose_arena = (QrVertex *)Mem_Alloc (sizeof (QrVertex) * alias_pose_arena_capacity);
	}
	else if (alias_pose_arena_capacity < ALIAS_POSE_ARENA_MAX_VERTS)
	{
		const int demand = (int)Atomic_LoadUInt32 (&alias_pose_arena_bump);

		if (demand > alias_pose_arena_capacity)
		{
			int capacity = alias_pose_arena_capacity;

			while (capacity < demand && capacity < ALIAS_POSE_ARENA_MAX_VERTS)
				capacity *= 2;

			if (capacity > ALIAS_POSE_ARENA_MAX_VERTS)
				capacity = ALIAS_POSE_ARENA_MAX_VERTS;

			QrVertex *arena = (QrVertex *)Mem_Alloc (sizeof (QrVertex) * capacity);

			Mem_Free (alias_pose_arena);
			alias_pose_arena = arena;
			alias_pose_arena_capacity = capacity;
		}
	}

	Atomic_StoreUInt32 (&alias_pose_arena_bump, 0);
	alias_pose_provider = provider;
	alias_pose_enabled = true;
}

int AliasPoseJobs_TaskLimit (void)
{
	return alias_pose_entry_capacity;
}

void AliasPoseJobs_PrepareTask (int index, void *unused)
{
	alias_pose_request_t request;
	alias_pose_entry_t  *entry;
	uint32_t             offset;
	double               prof_prep;
	double               prof_pose;

	if (!alias_pose_enabled)
		return;

	prof_prep = RT_Prof_Begin ();

	if (!alias_pose_provider (index, &request))
	{
		Atomic_IncrementUInt32 (&alias_pose_stat_declined);
		RT_Prof_End (RT_PROF_ALIAS_POSE_PREP, prof_prep);
		return;
	}

	offset = Atomic_AddUInt32 (&alias_pose_arena_bump, (uint32_t)request.vertex_count);

	if (offset + (uint32_t)request.vertex_count > (uint32_t)alias_pose_arena_capacity)
	{
		Atomic_IncrementUInt32 (&alias_pose_stat_overflow);
		RT_Prof_End (RT_PROF_ALIAS_POSE_PREP, prof_prep);
		return;
	}

	prof_pose = RT_Prof_Begin ();
	AliasPoseJobs_Kernel (request.pose1, request.pose2, alias_pose_arena + offset, 0, request.vertex_count,
	                      request.blend, request.cluster);
	RT_Prof_End (RT_PROF_ALIAS_POSE, prof_pose);

	entry = &alias_pose_entries[index];
	entry->pose1 = request.pose1;
	entry->pose2 = request.pose2;
	entry->vertices = alias_pose_arena + offset;
	entry->vertex_count = request.vertex_count;
	entry->blend = request.blend;
	entry->cluster = request.cluster;
	entry->serial = alias_pose_serial;

	Atomic_IncrementUInt32 (&alias_pose_stat_prepared);
	RT_Prof_End (RT_PROF_ALIAS_POSE_PREP, prof_prep);
}

qboolean AliasPoseJobs_Lookup (int index, const QrVertex *pose1, const QrVertex *pose2, int vertex_count,
                               float blend, int cluster, const QrVertex **out)
{
	const alias_pose_entry_t *entry;

	if (!alias_pose_enabled)
		return false;

	if (index < 0 || index >= alias_pose_entry_capacity)
	{
		Atomic_IncrementUInt32 (&alias_pose_stat_misses);
		return false;
	}

	entry = &alias_pose_entries[index];

	if (entry->serial == alias_pose_serial && entry->pose1 == pose1 && entry->pose2 == pose2 &&
	    entry->vertex_count == vertex_count && entry->blend == blend && entry->cluster == cluster)
	{
		Atomic_IncrementUInt32 (&alias_pose_stat_hits);
		*out = entry->vertices;
		return true;
	}

	Atomic_IncrementUInt32 (&alias_pose_stat_misses);
	return false;
}

void AliasPoseJobs_Disable (void)
{
	alias_pose_enabled = false;
	alias_pose_provider = NULL;
}

const alias_pose_stats_t *AliasPoseJobs_Stats (void)
{
	alias_pose_stats.prepared = Atomic_LoadUInt32 (&alias_pose_stat_prepared);
	alias_pose_stats.declined = Atomic_LoadUInt32 (&alias_pose_stat_declined);
	alias_pose_stats.overflow = Atomic_LoadUInt32 (&alias_pose_stat_overflow);
	alias_pose_stats.hits = Atomic_LoadUInt32 (&alias_pose_stat_hits);
	alias_pose_stats.misses = Atomic_LoadUInt32 (&alias_pose_stat_misses);
	return &alias_pose_stats;
}

void AliasPoseJobs_Shutdown (void)
{
	Mem_Free (alias_pose_arena);
	alias_pose_arena = NULL;
	alias_pose_arena_capacity = 0;

	Mem_Free (alias_pose_entries);
	alias_pose_entries = NULL;
	alias_pose_entry_capacity = 0;

	alias_pose_enabled = false;
	alias_pose_provider = NULL;
}
