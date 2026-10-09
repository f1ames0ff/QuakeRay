#ifndef ALIAS_POSE_JOBS_H
#define ALIAS_POSE_JOBS_H

#include "q_stdinc.h"
#include "qray/qray.h"

typedef void (*alias_pose_kernel_t)(const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst,
                                    int first, int count, float blend, int cluster);

typedef struct
{
	const QrVertex *pose1;
	const QrVertex *pose2;
	int             vertex_count;
	float           blend;
	int             cluster;
} alias_pose_request_t;

typedef qboolean (*alias_pose_provider_t)(int index, alias_pose_request_t *out);

typedef struct
{
	unsigned int prepared;
	unsigned int declined;
	unsigned int overflow;
	unsigned int hits;
	unsigned int misses;
} alias_pose_stats_t;

void                      AliasPoseJobs_Kernel (const QrVertex *pose1, const QrVertex *pose2, QrVertex *dst,
                                                int first, int count, float blend, int cluster);
qboolean                  AliasPoseJobs_NeedsCopy (float blend, int cluster);
void                      AliasPoseJobs_Begin (int visedict_capacity, alias_pose_provider_t provider);
int                       AliasPoseJobs_TaskLimit (void);
void                      AliasPoseJobs_PrepareTask (int index, void *unused);
qboolean                  AliasPoseJobs_Lookup (int index, const QrVertex *pose1, const QrVertex *pose2,
                                                int vertex_count, float blend, int cluster, const QrVertex **out);
void                      AliasPoseJobs_Disable (void);
const alias_pose_stats_t *AliasPoseJobs_Stats (void);
void                      AliasPoseJobs_Shutdown (void);

#endif
