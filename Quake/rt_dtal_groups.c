#include "rt_dtal_groups.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RT_DTAL_MAX_CLIP_VERTS 24
#define RT_DTAL_AREA_EPSILON 1e-6
#define RT_DTAL_AREA_RELATIVE 1e-7
#define RT_DTAL_CELL_LIMIT 2147483000.0

typedef struct rt_dtal_clipvert_s
{
    double p[3];
    double uv[2];
} rt_dtal_clipvert_t;

typedef struct rt_dtal_key_s
{
    uint64_t emissionKey;
    uint64_t serial;
    uint32_t styleKey;
    int32_t  cell[3];
    int32_t  axis;
} rt_dtal_key_t;

struct rt_dtal_builder_s
{
    rt_dtal_input_t  *inputs;
    int               inputCount;
    int               inputCapacity;

    rt_dtal_build_t   result;
    int               groupCapacity;
    int               memberCapacity;

    uint32_t         *keyTable;
    int               keyTableCapacity;

    uint64_t         *uidSet;
    int               uidSetCapacity;

    double           *weightScratch;

    int               failed;
};

static int RT_Dtal_EnsureMembers(rt_dtal_builder_t *builder, int needed)
{
    if (needed <= builder->memberCapacity)
        return 1;

    int newCapacity = builder->memberCapacity > 0 ? builder->memberCapacity : 1024;

    while (newCapacity < needed)
    {
        if (newCapacity > (1 << 28))
            return 0;
        newCapacity *= 2;
    }

    rt_dtal_member_t *members = (rt_dtal_member_t *)realloc(builder->result.members,
                                                             (size_t)newCapacity * sizeof(rt_dtal_member_t));
    float *prob = (float *)realloc(builder->result.memberProb, (size_t)newCapacity * sizeof(float));
    float *aliasProb = (float *)realloc(builder->result.memberAliasProb, (size_t)newCapacity * sizeof(float));
    uint32_t *alias = (uint32_t *)realloc(builder->result.memberAlias, (size_t)newCapacity * sizeof(uint32_t));
    double *weights = (double *)realloc(builder->weightScratch, (size_t)newCapacity * sizeof(double));

    if (members == NULL || prob == NULL || aliasProb == NULL || alias == NULL || weights == NULL)
    {
        if (members != NULL)
            builder->result.members = members;
        if (prob != NULL)
            builder->result.memberProb = prob;
        if (aliasProb != NULL)
            builder->result.memberAliasProb = aliasProb;
        if (alias != NULL)
            builder->result.memberAlias = alias;
        if (weights != NULL)
            builder->weightScratch = weights;
        return 0;
    }

    if (newCapacity > builder->memberCapacity)
    {
        memset(members + builder->memberCapacity, 0, (size_t)(newCapacity - builder->memberCapacity) * sizeof(rt_dtal_member_t));
        memset(prob + builder->memberCapacity, 0, (size_t)(newCapacity - builder->memberCapacity) * sizeof(float));
        memset(aliasProb + builder->memberCapacity, 0, (size_t)(newCapacity - builder->memberCapacity) * sizeof(float));
        memset(alias + builder->memberCapacity, 0, (size_t)(newCapacity - builder->memberCapacity) * sizeof(uint32_t));
        memset(weights + builder->memberCapacity, 0, (size_t)(newCapacity - builder->memberCapacity) * sizeof(double));
    }

    builder->result.members = members;
    builder->result.memberProb = prob;
    builder->result.memberAliasProb = aliasProb;
    builder->result.memberAlias = alias;
    builder->weightScratch = weights;
    builder->memberCapacity = newCapacity;

    return 1;
}

static uint64_t RT_Dtal_HashBytes(const void *data, int size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t             hash = 1469598103934665603ull;

    for (int i = 0; i < size; i++)
    {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static int RT_Dtal_GrowArray(void **array, int *capacity, int needed, int elementSize)
{
    if (needed <= *capacity)
        return 1;

    int newCapacity = *capacity > 0 ? *capacity : 1024;

    while (newCapacity < needed)
    {
        if (newCapacity > (1 << 28))
            return 0;
        newCapacity *= 2;
    }

    void *grown = realloc(*array, (size_t)newCapacity * (size_t)elementSize);

    if (grown == NULL)
        return 0;

    memset((unsigned char *)grown + (size_t)(*capacity) * (size_t)elementSize, 0,
           (size_t)(newCapacity - *capacity) * (size_t)elementSize);

    *array = grown;
    *capacity = newCapacity;

    return 1;
}

static int RT_Dtal_ClipPlane(const rt_dtal_clipvert_t *in, int count, int axis, double value, int keepLess,
                             rt_dtal_clipvert_t *out)
{
    int outCount = 0;

    for (int i = 0; i < count; i++)
    {
        const rt_dtal_clipvert_t *a = &in[i];
        const rt_dtal_clipvert_t *b = &in[(i + 1) % count];
        const double              da = a->p[axis] - value;
        const double              db = b->p[axis] - value;
        const int                 ina = keepLess ? (da < 0.0) : (da >= 0.0);
        const int                 inb = keepLess ? (db < 0.0) : (db >= 0.0);

        if (ina)
        {
            if (outCount >= RT_DTAL_MAX_CLIP_VERTS)
                return -1;
            out[outCount++] = *a;
        }

        if (ina != inb)
        {
            const double t = da / (da - db);

            if (!isfinite(t) || t < 0.0 || t > 1.0)
                continue;

            if (outCount >= RT_DTAL_MAX_CLIP_VERTS)
                return -1;

            out[outCount].p[0] = a->p[0] + (b->p[0] - a->p[0]) * t;
            out[outCount].p[1] = a->p[1] + (b->p[1] - a->p[1]) * t;
            out[outCount].p[2] = a->p[2] + (b->p[2] - a->p[2]) * t;
            out[outCount].uv[0] = a->uv[0] + (b->uv[0] - a->uv[0]) * t;
            out[outCount].uv[1] = a->uv[1] + (b->uv[1] - a->uv[1]) * t;
            outCount++;
        }
    }

    return outCount;
}

static double RT_Dtal_TriangleArea(const rt_dtal_clipvert_t *a, const rt_dtal_clipvert_t *b, const rt_dtal_clipvert_t *c)
{
    const double e1[3] = { b->p[0] - a->p[0], b->p[1] - a->p[1], b->p[2] - a->p[2] };
    const double e2[3] = { c->p[0] - a->p[0], c->p[1] - a->p[1], c->p[2] - a->p[2] };
    const double cross[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0],
    };

    return 0.5 * sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
}

static int32_t RT_Dtal_DominantAxis(const float normal[3])
{
    const double ax = fabs((double)normal[0]);
    const double ay = fabs((double)normal[1]);
    const double az = fabs((double)normal[2]);

    if (ax >= ay && ax >= az)
        return normal[0] >= 0.0f ? 0 : 1;
    if (ay >= az)
        return normal[1] >= 0.0f ? 2 : 3;
    return normal[2] >= 0.0f ? 4 : 5;
}

static double RT_Dtal_ClampWeight(float weight)
{
    if (!isfinite(weight) || weight < 0.0f)
        return 0.0;
    return weight > 1.0f ? 1.0 : (double)weight;
}

static int RT_Dtal_KeyTableInsert(rt_dtal_builder_t *builder, const rt_dtal_key_t *key, int groupIndex, int *outIndex);

static int RT_Dtal_EnsureKeyTable(rt_dtal_builder_t *builder, int groupCount)
{
    int capacity = builder->keyTableCapacity > 0 ? builder->keyTableCapacity : 1024;

    while (capacity < groupCount * 2)
    {
        if (capacity > (1 << 27))
            return 0;
        capacity *= 2;
    }

    if (capacity == builder->keyTableCapacity)
        return 1;

    uint32_t *table = (uint32_t *)calloc((size_t)capacity, sizeof(uint32_t));

    if (table == NULL)
        return 0;

    uint32_t *oldTable = builder->keyTable;
    const int oldCapacity = builder->keyTableCapacity;

    builder->keyTable = table;
    builder->keyTableCapacity = capacity;

    if (oldTable != NULL)
    {
        for (int i = 0; i < oldCapacity; i++)
        {
            if (oldTable[i] != 0u)
            {
                int group = (int)oldTable[i] - 1;
                const rt_dtal_group_t *g = &builder->result.groups[group];
                rt_dtal_key_t          key;
                memset(&key, 0, sizeof(key));
                key.emissionKey = g->emissionKey;
                key.styleKey = g->styleKey;
                key.serial = g->serial;
                key.axis = g->axis;
                key.cell[0] = g->cell[0];
                key.cell[1] = g->cell[1];
                key.cell[2] = g->cell[2];

                int ignored = 0;
                RT_Dtal_KeyTableInsert(builder, &key, group, &ignored);
            }
        }

        free(oldTable);
    }

    return 1;
}

static int RT_Dtal_KeyTableInsert(rt_dtal_builder_t *builder, const rt_dtal_key_t *key, int groupIndex, int *outIndex)
{
    const uint64_t hash = RT_Dtal_HashBytes(key, (int)sizeof(*key));
    const uint32_t mask = (uint32_t)builder->keyTableCapacity - 1u;
    uint32_t       slot = (uint32_t)(hash >> 20) & mask;

    for (int probe = 0; probe < builder->keyTableCapacity; probe++)
    {
        const uint32_t entry = builder->keyTable[slot];

        if (entry == 0u)
        {
            builder->keyTable[slot] = (uint32_t)groupIndex + 1u;
            return 1;
        }

        const rt_dtal_group_t *g = &builder->result.groups[(int)entry - 1];

        rt_dtal_key_t stored;
        memset(&stored, 0, sizeof(stored));
        stored.emissionKey = g->emissionKey;
        stored.styleKey = g->styleKey;
        stored.serial = g->serial;
        stored.axis = g->axis;
        stored.cell[0] = g->cell[0];
        stored.cell[1] = g->cell[1];
        stored.cell[2] = g->cell[2];

        if (memcmp(&stored, key, sizeof(*key)) == 0)
        {
            if (outIndex != NULL)
                *outIndex = (int)entry - 1;
            return 0;
        }

        slot = (slot + 1u) & mask;
    }

    return -1;
}

static int RT_Dtal_FindOrAddGroup(rt_dtal_builder_t *builder, const rt_dtal_key_t *key, uint64_t sourceIndex,
                                  uint64_t sourceUid, int *outIndex)
{
    if (builder->keyTable == NULL || builder->result.groupCount * 2 >= builder->keyTableCapacity)
    {
        if (!RT_Dtal_EnsureKeyTable(builder, builder->result.groupCount + 1))
            return 0;
    }

    const int found = RT_Dtal_KeyTableInsert(builder, key, builder->result.groupCount, outIndex);

    if (found < 0)
        return 0;
    if (found > 0)
    {
        if (builder->result.groupCount >= RT_DTAL_MAX_GROUPS)
            return 0;

        if (!RT_Dtal_GrowArray((void **)&builder->result.groups, &builder->groupCapacity,
                               builder->result.groupCount + 1, (int)sizeof(rt_dtal_group_t)))
        {
            return 0;
        }

        rt_dtal_group_t *group = &builder->result.groups[builder->result.groupCount];
        memset(group, 0, sizeof(*group));
        group->emissionKey = key->emissionKey;
        group->styleKey = key->styleKey;
        group->sourceIndex = sourceIndex;
        group->sourceUid = sourceUid;
        group->serial = key->serial;
        group->cell[0] = key->cell[0];
        group->cell[1] = key->cell[1];
        group->cell[2] = key->cell[2];
        group->axis = key->axis;
        group->firstMember = builder->result.memberCount;
        group->memberCount = 0;
        group->mins[0] = group->mins[1] = group->mins[2] = 0.0f;
        group->maxs[0] = group->maxs[1] = group->maxs[2] = 0.0f;

        *outIndex = builder->result.groupCount;
        builder->result.groupCount++;
    }

    return 1;
}

static int RT_Dtal_AddMember(rt_dtal_builder_t *builder, int groupIndex, const rt_dtal_input_t *input,
                             const rt_dtal_clipvert_t *poly, int count, double area)
{
    if (builder->result.memberCount >= RT_DTAL_MAX_MEMBERS)
        return 0;

    if (!RT_Dtal_EnsureMembers(builder, builder->result.memberCount + 1))
        return 0;

    rt_dtal_member_t *member = &builder->result.members[builder->result.memberCount];
    memset(member, 0, sizeof(*member));

    for (int k = 0; k < 3; k++)
    {
        member->A[k] = input->axisU[k];
        member->B[k] = input->axisV[k];
        member->C[k] = input->origin[k];
        member->normal[k] = input->normal[k];
    }

    for (int i = 0; i < count; i++)
    {
        member->uv[i][0] = (float)poly[i].uv[0];
        member->uv[i][1] = (float)poly[i].uv[1];

        member->center[0] += (float)(poly[i].p[0] / (double)count);
        member->center[1] += (float)(poly[i].p[1] / (double)count);
        member->center[2] += (float)(poly[i].p[2] / (double)count);
    }

    member->numVerts = count;
    member->area = (float)area;
    member->refWeight = (float)RT_Dtal_ClampWeight(input->referenceWeight);
    member->group = groupIndex;
    member->sourceUid = input->uid;

    rt_dtal_group_t *group = &builder->result.groups[groupIndex];

    if (group->memberCount == 0)
    {
        for (int k = 0; k < 3; k++)
        {
            group->mins[k] = (float)poly[0].p[k];
            group->maxs[k] = (float)poly[0].p[k];
        }
    }

    for (int i = 0; i < count; i++)
    {
        for (int k = 0; k < 3; k++)
        {
            group->mins[k] = fminf(group->mins[k], (float)poly[i].p[k]);
            group->maxs[k] = fmaxf(group->maxs[k], (float)poly[i].p[k]);
        }
    }

    group->memberCount++;
    group->area += (float)area;
    group->refPower += (float)(area * RT_Dtal_ClampWeight(input->referenceWeight) *
                               (isfinite(input->radiantPower) && input->radiantPower > 0.0f ? input->radiantPower : 0.0f));

    builder->result.memberCount++;
    builder->result.diag.patchArea += area;

    return 1;
}

static int RT_Dtal_AddSingleton(rt_dtal_builder_t *builder, const rt_dtal_input_t *input, int index, double spacing)
{
    rt_dtal_clipvert_t poly[RT_DTAL_MAX_VERTS];

    for (int i = 0; i < input->numVerts; i++)
    {
        poly[i].uv[0] = input->uv[i][0];
        poly[i].uv[1] = input->uv[i][1];

        for (int k = 0; k < 3; k++)
        {
            const double u = (double)input->uv[i][0];
            const double v = (double)input->uv[i][1];
            poly[i].p[k] = (double)input->origin[k] + (double)input->axisU[k] * u + (double)input->axisV[k] * v;
        }
    }

    double area = 0.0;

    for (int i = 1; i + 1 < input->numVerts; i++)
        area += RT_Dtal_TriangleArea(&poly[0], &poly[i], &poly[i + 1]);

    if (area <= RT_DTAL_AREA_EPSILON)
    {
        builder->result.diag.droppedDegenerate++;
        builder->result.diag.droppedArea += (double)input->area;
        return 1;
    }

    rt_dtal_key_t key;
    memset(&key, 0, sizeof(key));
    key.emissionKey = input->emissionKey;
    key.styleKey = input->styleKey;
    key.serial = input->uid ^ ((uint64_t)index << 40);
    key.axis = RT_Dtal_DominantAxis(input->normal);
    key.cell[0] = (int32_t)floor((double)poly[0].p[0] / spacing);
    key.cell[1] = (int32_t)floor((double)poly[0].p[1] / spacing);
    key.cell[2] = (int32_t)floor((double)poly[0].p[2] / spacing);

    int groupIndex = -1;

    if (!RT_Dtal_FindOrAddGroup(builder, &key, input->sourceIndex, input->uid, &groupIndex))
        return 0;

    if (!RT_Dtal_AddMember(builder, groupIndex, input, poly, input->numVerts, area))
        return 0;

    builder->result.diag.clippedPatches++;

    return 1;
}

static int RT_Dtal_AddClipped(rt_dtal_builder_t *builder, const rt_dtal_input_t *input, double spacing)
{
    rt_dtal_clipvert_t base[RT_DTAL_MAX_VERTS];

    double mins[3] = { 1e30, 1e30, 1e30 };
    double maxs[3] = { -1e30, -1e30, -1e30 };

    for (int i = 0; i < input->numVerts; i++)
    {
        base[i].uv[0] = input->uv[i][0];
        base[i].uv[1] = input->uv[i][1];

        for (int k = 0; k < 3; k++)
        {
            const double u = (double)input->uv[i][0];
            const double v = (double)input->uv[i][1];
            base[i].p[k] = (double)input->origin[k] + (double)input->axisU[k] * u + (double)input->axisV[k] * v;

            mins[k] = fmin(mins[k], base[i].p[k]);
            maxs[k] = fmax(maxs[k], base[i].p[k]);
        }
    }

    const double cellMinD[3] = { floor(mins[0] / spacing), floor(mins[1] / spacing), floor(mins[2] / spacing) };
    const double cellMaxD[3] = { floor(maxs[0] / spacing), floor(maxs[1] / spacing), floor(maxs[2] / spacing) };

    if (cellMinD[0] < -RT_DTAL_CELL_LIMIT || cellMinD[1] < -RT_DTAL_CELL_LIMIT || cellMinD[2] < -RT_DTAL_CELL_LIMIT ||
        cellMaxD[0] > RT_DTAL_CELL_LIMIT || cellMaxD[1] > RT_DTAL_CELL_LIMIT || cellMaxD[2] > RT_DTAL_CELL_LIMIT)
    {
        builder->result.diag.oversizedPieces++;
        return -1;
    }

    const double cellsX = cellMaxD[0] - cellMinD[0] + 1.0;
    const double cellsY = cellMaxD[1] - cellMinD[1] + 1.0;
    const double cellsZ = cellMaxD[2] - cellMinD[2] + 1.0;

    if (cellsX * cellsY * cellsZ > (double)RT_DTAL_MAX_CELL_VISITS_PER_PIECE)
    {
        builder->result.diag.oversizedPieces++;
        return -1;
    }

    const int32_t axis = RT_Dtal_DominantAxis(input->normal);
    const double  tolerance = fmax(RT_DTAL_AREA_EPSILON, (double)input->area * RT_DTAL_AREA_RELATIVE);

    for (int32_t cz = (int32_t)cellMinD[2]; cz <= (int32_t)cellMaxD[2]; cz++)
    {
        for (int32_t cy = (int32_t)cellMinD[1]; cy <= (int32_t)cellMaxD[1]; cy++)
        {
            for (int32_t cx = (int32_t)cellMinD[0]; cx <= (int32_t)cellMaxD[0]; cx++)
            {
                const double boxMin[3] = { (double)cx * spacing, (double)cy * spacing, (double)cz * spacing };
                const double boxMax[3] = { boxMin[0] + spacing, boxMin[1] + spacing, boxMin[2] + spacing };

                rt_dtal_clipvert_t work[2][RT_DTAL_MAX_CLIP_VERTS];
                int                count = input->numVerts;

                memcpy(work[0], base, (size_t)count * sizeof(rt_dtal_clipvert_t));

                int failed = 0;

                for (int axisIndex = 0; axisIndex < 3 && !failed; axisIndex++)
                {
                    const int     keepLess = 0;
                    const int     result = RT_Dtal_ClipPlane(work[0], count, axisIndex, boxMin[axisIndex], keepLess, work[1]);

                    if (result < 0)
                    {
                        failed = 1;
                        break;
                    }

                    memcpy(work[0], work[1], (size_t)result * sizeof(rt_dtal_clipvert_t));
                    count = result;

                    if (count < 3)
                        break;

                    const int upper = RT_Dtal_ClipPlane(work[0], count, axisIndex, boxMax[axisIndex], 1, work[1]);

                    if (upper < 0)
                    {
                        failed = 1;
                        break;
                    }

                    memcpy(work[0], work[1], (size_t)upper * sizeof(rt_dtal_clipvert_t));
                    count = upper;

                    if (count < 3)
                        break;
                }

                builder->result.diag.cellVisits++;

                if (failed || count < 3)
                {
                    builder->result.diag.emptyClips++;
                    continue;
                }

                double total = 0.0;

                for (int i = 1; i + 1 < count; i++)
                    total += RT_Dtal_TriangleArea(&work[0][0], &work[0][i], &work[0][i + 1]);

                if (total <= tolerance)
                {
                    builder->result.diag.droppedDegenerate++;
                    continue;
                }

                rt_dtal_key_t key;
                memset(&key, 0, sizeof(key));
                key.emissionKey = input->emissionKey;
                key.styleKey = input->styleKey;
                key.axis = axis;
                key.cell[0] = cx;
                key.cell[1] = cy;
                key.cell[2] = cz;

                int groupIndex = -1;

                if (!RT_Dtal_FindOrAddGroup(builder, &key, input->sourceIndex, input->uid, &groupIndex))
                    return 0;

                for (int i = 1; i + 1 < count; i++)
                {
                    const double area = RT_Dtal_TriangleArea(&work[0][0], &work[0][i], &work[0][i + 1]);

                    if (area <= tolerance)
                    {
                        builder->result.diag.droppedDegenerate++;
                        continue;
                    }

                    const rt_dtal_clipvert_t triangle[3] = { work[0][0], work[0][i], work[0][i + 1] };

                    if (!RT_Dtal_AddMember(builder, groupIndex, input, triangle, 3, area))
                        return 0;

                    builder->result.diag.clippedPatches++;
                }
            }
        }
    }

    return 1;
}

static int RT_Dtal_CompactMembers(rt_dtal_builder_t *builder)
{
    const int total = builder->result.memberCount;
    const int groups = builder->result.groupCount;

    if (total <= 0 || groups <= 0)
        return 1;

    int *next = (int *)calloc((size_t)groups, sizeof(int));

    if (next == NULL)
        return 0;

    int offset = 0;

    for (int g = 0; g < groups; g++)
    {
        builder->result.groups[g].firstMember = offset;
        next[g] = offset;
        offset += builder->result.groups[g].memberCount;
    }

    if (offset != total)
    {
        free(next);
        return 0;
    }

    rt_dtal_member_t *members = (rt_dtal_member_t *)malloc((size_t)total * sizeof(rt_dtal_member_t));
    float *prob = (float *)malloc((size_t)total * sizeof(float));
    float *aliasProb = (float *)malloc((size_t)total * sizeof(float));
    uint32_t *alias = (uint32_t *)malloc((size_t)total * sizeof(uint32_t));

    if (members == NULL || prob == NULL || aliasProb == NULL || alias == NULL)
    {
        free(members);
        free(prob);
        free(aliasProb);
        free(alias);
        free(next);
        return 0;
    }

    for (int i = 0; i < total; i++)
    {
        const rt_dtal_member_t *member = &builder->result.members[i];
        const int g = member->group;

        if (g < 0 || g >= groups)
        {
            free(members);
            free(prob);
            free(aliasProb);
            free(alias);
            free(next);
            return 0;
        }

        const int dst = next[g]++;
        members[dst] = *member;
        prob[dst] = 0.0f;
        aliasProb[dst] = 0.0f;
        alias[dst] = 0u;
    }

    free(builder->result.members);
    free(builder->result.memberProb);
    free(builder->result.memberAliasProb);
    free(builder->result.memberAlias);
    free(next);

    builder->result.members = members;
    builder->result.memberProb = prob;
    builder->result.memberAliasProb = aliasProb;
    builder->result.memberAlias = alias;
    builder->memberCapacity = total;

    return 1;
}

static int RT_Dtal_UidSetContains(const uint64_t *set, int capacity, uint64_t uid)
{
    if (capacity <= 0)
        return 0;

    const uint32_t mask = (uint32_t)capacity - 1u;
    uint32_t       slot = (uint32_t)(RT_Dtal_HashBytes(&uid, (int)sizeof(uid)) >> 20) & mask;

    for (int probe = 0; probe < capacity; probe++)
    {
        if (set[slot] == 0ull)
            return 0;
        if (set[slot] == uid)
            return 1;

        slot = (slot + 1u) & mask;
    }

    return 0;
}

static void RT_Dtal_UidSetInsert(uint64_t *set, int capacity, uint64_t uid)
{
    const uint32_t mask = (uint32_t)capacity - 1u;
    uint32_t       slot = (uint32_t)(RT_Dtal_HashBytes(&uid, (int)sizeof(uid)) >> 20) & mask;

    while (set[slot] != 0ull)
        slot = (slot + 1u) & mask;

    set[slot] = uid;
}

static int RT_Dtal_AssignUids(rt_dtal_builder_t *builder)
{
    int capacity = 1024;

    while (capacity < builder->result.groupCount * 2)
    {
        if (capacity > (1 << 27))
            return 0;
        capacity *= 2;
    }

    free(builder->uidSet);
    builder->uidSet = NULL;
    builder->uidSetCapacity = 0;

    uint64_t *set = (uint64_t *)calloc((size_t)capacity, sizeof(uint64_t));

    if (set == NULL)
        return 0;

    builder->uidSet = set;
    builder->uidSetCapacity = capacity;

    for (int i = 0; i < builder->result.groupCount; i++)
    {
        const rt_dtal_group_t *group = &builder->result.groups[i];

        rt_dtal_key_t key;
        memset(&key, 0, sizeof(key));
        key.emissionKey = group->emissionKey;
        key.styleKey = group->styleKey;
        key.serial = group->serial;
        key.axis = group->axis;
        key.cell[0] = group->cell[0];
        key.cell[1] = group->cell[1];
        key.cell[2] = group->cell[2];

        uint64_t uid = (4ull << 60) | (RT_Dtal_HashBytes(&key, (int)sizeof(key)) & ((1ull << 60) - 1ull));

        while (RT_Dtal_UidSetContains(set, capacity, uid))
        {
            uid = (4ull << 60) | ((uid + 1ull) & ((1ull << 60) - 1ull));
        }

        RT_Dtal_UidSetInsert(set, capacity, uid);
        builder->result.groups[i].uid = uid;
    }

    return 1;
}

static void RT_Dtal_BuildAliases(rt_dtal_builder_t *builder)
{
    for (int gi = 0; gi < builder->result.groupCount; gi++)
    {
        rt_dtal_group_t *group = &builder->result.groups[gi];
        const int        first = group->firstMember;
        const int        count = group->memberCount;

        for (int i = 0; i < count; i++)
        {
            const rt_dtal_member_t *member = &builder->result.members[first + i];
            const double            weight = fmax((double)member->area, 1e-12);
            const double            mask = RT_Dtal_ClampWeight(member->refWeight);
            builder->weightScratch[i] = weight * (RT_ALIAS_UNIFORM_PRIOR + (1.0 - RT_ALIAS_UNIFORM_PRIOR) * mask);
        }

        if (count > 0)
        {
            if (!RT_Alias_Build(builder->weightScratch, count, &builder->result.memberProb[first],
                                &builder->result.memberAliasProb[first], &builder->result.memberAlias[first]))
            {
                for (int i = 0; i < count; i++)
                {
                    builder->result.memberProb[first + i] = 1.0f;
                    builder->result.memberAliasProb[first + i] = 0.0f;
                    builder->result.memberAlias[first + i] = 0u;
                }
            }
        }
    }
}

static void RT_Dtal_FinishBounds(rt_dtal_builder_t *builder)
{
    for (int gi = 0; gi < builder->result.groupCount; gi++)
    {
        rt_dtal_group_t *group = &builder->result.groups[gi];

        group->center[0] = 0.5f * (group->mins[0] + group->maxs[0]);
        group->center[1] = 0.5f * (group->mins[1] + group->maxs[1]);
        group->center[2] = 0.5f * (group->mins[2] + group->maxs[2]);

        float radius = 0.0f;

        for (int k = 0; k < 3; k++)
        {
            const float half = 0.5f * (group->maxs[k] - group->mins[k]);

            radius += half * half;
        }

        group->boundsRadius = sqrtf(radius);
    }
}

rt_dtal_builder_t *RT_Dtal_BuilderCreate(void)
{
    rt_dtal_builder_t *builder = (rt_dtal_builder_t *)calloc(1, sizeof(rt_dtal_builder_t));

    return builder;
}

void RT_Dtal_BuilderResetInputs(rt_dtal_builder_t *builder)
{
    if (builder != NULL)
        builder->inputCount = 0;
}

void RT_Dtal_BuilderDestroy(rt_dtal_builder_t *builder)
{
    if (builder == NULL)
        return;

    free(builder->inputs);
    free(builder->result.groups);
    free(builder->result.members);
    free(builder->result.memberProb);
    free(builder->result.memberAliasProb);
    free(builder->result.memberAlias);
    free(builder->keyTable);
    free(builder->uidSet);
    free(builder->weightScratch);
    free(builder);
}

int RT_Dtal_BuilderAddInput(rt_dtal_builder_t *builder, const rt_dtal_input_t *input)
{
    if (builder == NULL || input == NULL)
        return 0;

    if (input->numVerts < 3 || input->numVerts > RT_DTAL_MAX_VERTS)
        return 0;

    if (!(input->area > 0.0f) || !isfinite(input->area))
        return 0;

    for (int i = 0; i < 3; i++)
    {
        if (!isfinite(input->origin[i]) || !isfinite(input->axisU[i]) || !isfinite(input->axisV[i]) ||
            !isfinite(input->normal[i]))
        {
            return 0;
        }
    }

    for (int i = 0; i < input->numVerts; i++)
    {
        if (!isfinite(input->uv[i][0]) || !isfinite(input->uv[i][1]))
            return 0;
    }

    if (!isfinite(input->referenceWeight) || !isfinite(input->radiantPower))
        return 0;

    if (builder->inputCount >= RT_DTAL_MAX_INPUTS)
    {
        builder->result.diag.budgetRefused++;
        return 0;
    }

    if (!RT_Dtal_GrowArray((void **)&builder->inputs, &builder->inputCapacity, builder->inputCount + 1,
                           (int)sizeof(rt_dtal_input_t)))
    {
        return 0;
    }

    builder->inputs[builder->inputCount++] = *input;

    return 1;
}

int RT_Dtal_BuilderBuild(rt_dtal_builder_t *builder, double spacing, int singleton)
{
    if (builder == NULL)
        return 0;

    if (!isfinite(spacing) || !(spacing > 0.0) || spacing > 1.0e9)
        return 0;

    builder->result.groupCount = 0;
    builder->result.memberCount = 0;
    memset(&builder->result.diag, 0, sizeof(builder->result.diag));
    builder->failed = 0;

    if (builder->keyTable != NULL)
        memset(builder->keyTable, 0, (size_t)builder->keyTableCapacity * sizeof(uint32_t));
    if (builder->uidSet != NULL)
        memset(builder->uidSet, 0, (size_t)builder->uidSetCapacity * sizeof(uint64_t));

    for (int i = 0; i < builder->inputCount; i++)
    {
        const rt_dtal_input_t *input = &builder->inputs[i];

        builder->result.diag.inputPieces++;
        builder->result.diag.inputArea += (double)input->area;

        int ok;

        if (singleton)
        {
            ok = RT_Dtal_AddSingleton(builder, input, i, spacing);
        }
        else
        {
            ok = RT_Dtal_AddClipped(builder, input, spacing);

            if (ok < 0)
            {
                ok = RT_Dtal_AddSingleton(builder, input, i, spacing);
            }
        }

        if (!ok)
        {
            builder->result.diag.budgetRefused++;
            builder->failed = 1;
            break;
        }
    }

    if (builder->failed)
        return 0;

    if (!RT_Dtal_CompactMembers(builder))
        return 0;

    if (!RT_Dtal_AssignUids(builder))
        return 0;

    RT_Dtal_FinishBounds(builder);
    RT_Dtal_BuildAliases(builder);

    return 1;
}

const rt_dtal_build_t *RT_Dtal_BuilderResult(const rt_dtal_builder_t *builder)
{
    return builder != NULL ? &builder->result : NULL;
}
