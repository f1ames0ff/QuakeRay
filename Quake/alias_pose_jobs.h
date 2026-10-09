#ifndef ALIAS_POSE_JOBS_H
#define ALIAS_POSE_JOBS_H

#include "q_stdinc.h"
#include "qray/qray.h"

typedef void (*alias_pose_kernel_t)(const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst,
                                    int first, int count, float blend, int cluster);

typedef enum
{
	ALIAS_POSE_OWNER_BORROWED = 0,
	ALIAS_POSE_OWNER_SLOT,
} alias_pose_owner_t;

typedef struct
{
	const QrVertex    *vertices;
	int                vertex_count;
	int                slot;
	alias_pose_owner_t owner;
} alias_pose_result_t;

typedef struct
{
	double phase_ms;
	double worker_ms;
	int    jobs;
	int    chunks;
	int    borrowed;
	int    slot_misses;
} alias_pose_stats_t;

void                      AliasPoseJobs_Begin (qboolean parallel);
qboolean                  AliasPoseJobs_Prepare (const QrVertex *pose1, const QrVertex *pose2,
                                                 int vertex_count, float blend, int cluster,
                                                 alias_pose_kernel_t kernel, alias_pose_result_t *result);
void                      AliasPoseJobs_Join (void);
void                      AliasPoseJobs_Release (alias_pose_result_t *result);
const alias_pose_stats_t *AliasPoseJobs_Stats (void);
void                      AliasPoseJobs_Shutdown (void);

#endif
