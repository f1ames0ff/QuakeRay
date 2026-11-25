// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#ifndef RT_CLUSTER_SELECT_H
#define RT_CLUSTER_SELECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_CLUSTER_MAX_FAST 128
#define RT_CLUSTER_FAST_STRATUM 16
#define RT_CLUSTER_NEAREST_RESERVE 16
#define RT_CLUSTER_TAIL_BUDGET 4194304

typedef struct rt_cluster_candidate_s
{
    uint64_t uid;
    float    power;
    float    distanceSquared;
    float    scaleSquared;
} rt_cluster_candidate_t;

typedef struct rt_cluster_select_s
{
    int       fastCount;
    uint32_t  fastIndex[RT_CLUSTER_MAX_FAST];
    int       tailCount;
    uint32_t *tailIndex;
    float    *tailProb;
    float    *tailMarginal;
    uint32_t *tailAlias;
    float     beta;
    int       tailCapacity;
    int       overflow;
} rt_cluster_select_t;

int  RT_ClusterSelect_Build(rt_cluster_select_t *select, const rt_cluster_candidate_t *candidates, int count,
                            int repairedRanking);
void RT_ClusterSelect_Free(rt_cluster_select_t *select);

int RT_ClusterSelect_FastSelect(const rt_cluster_select_t *select, const double *fastMasses, float u0,
                                uint32_t *outSlot, float *outProbability);

#ifdef __cplusplus
}
#endif

#endif
