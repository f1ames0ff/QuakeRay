#ifndef RT_DTAL_LOD_H
#define RT_DTAL_LOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_DTAL_LOD_MAX_BANDS 16
#define RT_DTAL_LOD_NO_NODE 0xFFFFFFFFu

typedef enum
{
    RT_DTAL_LOD_VISIBLE = 0,
    RT_DTAL_LOD_OFF_FRUSTUM = 1,
    RT_DTAL_LOD_NOT_IN_ROWS = 2,
    RT_DTAL_LOD_VISIBILITY_COUNT = 3
} rt_dtal_lod_visibility_t;

typedef struct rt_dtal_lod_config_s
{
    float startDistance;
    float screenAngleDeg;
    int   maxBands;
    int   offscreenShift;
    float hysteresis;
    float dwellSeconds;
} rt_dtal_lod_config_t;

typedef struct rt_dtal_lod_input_s
{
    uint64_t uid;
    uint64_t emissionKey;
    uint32_t styleKey;
    int32_t  axis;
    uint32_t passClass;
    float    origin[3];
    float    normal[3];
    float    area;
    float    power;
    float    boundsRadius;
} rt_dtal_lod_input_t;

typedef struct rt_dtal_lod_node_s
{
    uint64_t uid;
    int32_t  band;
    int32_t  cell[3];
    int32_t  axis;
    uint64_t emissionKey;
    uint32_t styleKey;
    uint32_t passClass;
    uint32_t firstMember;
    uint32_t memberCount;
    float    center[3];
    float    normal[3];
    float    area;
    float    power;
    float    boundsRadius;
} rt_dtal_lod_node_t;

typedef struct rt_dtal_lod_stats_s
{
    uint64_t groups;
    uint64_t mergedGroups;
    uint64_t individualGroups;
    uint64_t publishedNodes;
    uint64_t degenerateRefused;
    uint64_t transitions;
    uint64_t bandGroups[RT_DTAL_LOD_MAX_BANDS];
} rt_dtal_lod_stats_t;

typedef struct rt_dtal_lod_s rt_dtal_lod_t;

rt_dtal_lod_t *RT_Dtal_Lod_Create(void);
void RT_Dtal_Lod_Destroy(rt_dtal_lod_t *lod);

int RT_Dtal_Lod_Build(rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config,
                      const rt_dtal_lod_input_t *inputs, int count);
void RT_Dtal_Lod_Reset(rt_dtal_lod_t *lod);

int RT_Dtal_Lod_Select(rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config,
                       const rt_dtal_lod_input_t *inputs, int count,
                       const uint32_t *visibility, const float camera[3],
                       double now, int budget, int *cursor);

uint32_t RT_Dtal_Lod_NodeCount(const rt_dtal_lod_t *lod);
const rt_dtal_lod_node_t *RT_Dtal_Lod_Nodes(const rt_dtal_lod_t *lod);
const uint32_t *RT_Dtal_Lod_Members(const rt_dtal_lod_t *lod);
uint32_t RT_Dtal_Lod_GroupNode(const rt_dtal_lod_t *lod, int groupIndex);
int RT_Dtal_Lod_GroupCount(const rt_dtal_lod_t *lod);
const rt_dtal_lod_stats_t *RT_Dtal_Lod_Stats(const rt_dtal_lod_t *lod);

#ifdef __cplusplus
}
#endif

#endif
