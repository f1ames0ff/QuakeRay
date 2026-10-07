#ifndef RT_DTAL_GROUPS_H
#define RT_DTAL_GROUPS_H

#include <stdint.h>

#include "rt_alias.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_DTAL_MAX_VERTS 8

#define RT_DTAL_MAX_GROUPS 65536
#define RT_DTAL_MAX_MEMBERS 262144
#define RT_DTAL_MAX_INPUTS 262144
#define RT_DTAL_MAX_CELL_VISITS_PER_PIECE 4096

typedef struct rt_dtal_input_s
{
    uint64_t uid;
    uint64_t emissionKey;
    uint64_t sourceIndex;
    uint32_t styleKey;
    float    origin[3];
    float    axisU[3];
    float    axisV[3];
    float    normal[3];
    float    uv[RT_DTAL_MAX_VERTS][2];
    int      numVerts;
    float    area;
    float    referenceWeight;
    float    radiantPower;
} rt_dtal_input_t;

typedef struct rt_dtal_member_s
{
    float    A[3];
    float    B[3];
    float    C[3];
    float    normal[3];
    float    uv[RT_DTAL_MAX_VERTS][2];
    float    center[3];
    int      numVerts;
    float    area;
    float    refWeight;
    int32_t  group;
    uint64_t sourceUid;
} rt_dtal_member_t;

typedef struct rt_dtal_group_s
{
    uint64_t uid;
    uint64_t emissionKey;
    uint64_t sourceIndex;
    uint64_t sourceUid;
    uint64_t serial;
    uint32_t styleKey;
    int32_t  cell[3];
    int32_t  axis;
    int32_t  firstMember;
    int32_t  memberCount;
    float    mins[3];
    float    maxs[3];
    float    center[3];
    float    boundsRadius;
    float    area;
    float    refPower;
} rt_dtal_group_t;

typedef struct rt_dtal_diag_s
{
    uint64_t inputPieces;
    uint64_t clippedPatches;
    uint64_t oversizedPieces;
    uint64_t emptyClips;
    uint64_t droppedDegenerate;
    uint64_t budgetRefused;
    uint64_t cellVisits;
    uint64_t maxCellVisitsPerPiece;
    double   inputArea;
    double   patchArea;
    double   droppedArea;
} rt_dtal_diag_t;

typedef struct rt_dtal_build_s
{
    rt_dtal_group_t  *groups;
    int               groupCount;
    rt_dtal_member_t *members;
    int               memberCount;
    float            *memberProb;
    float            *memberMarginal;
    uint32_t         *memberAlias;
    rt_dtal_diag_t    diag;
} rt_dtal_build_t;

typedef struct rt_dtal_builder_s rt_dtal_builder_t;

rt_dtal_builder_t *RT_Dtal_BuilderCreate(void);
void RT_Dtal_BuilderDestroy(rt_dtal_builder_t *builder);
void RT_Dtal_BuilderResetInputs(rt_dtal_builder_t *builder);
int  RT_Dtal_BuilderAddInput(rt_dtal_builder_t *builder, const rt_dtal_input_t *input);
int  RT_Dtal_BuilderBuild(rt_dtal_builder_t *builder, double spacing, int singleton);
const rt_dtal_build_t *RT_Dtal_BuilderResult(const rt_dtal_builder_t *builder);

#ifdef __cplusplus
}
#endif

#endif
