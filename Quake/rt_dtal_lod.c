#include "rt_dtal_lod.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define RT_DTAL_LOD_CELL_LIMIT    2147483000.0
#define RT_DTAL_LOD_POWER_EPSILON 1e-9
#define RT_DTAL_LOD_AREA_EPSILON  1e-8
#define RT_DTAL_LOD_NORMAL_REL    1e-6
#define RT_DTAL_LOD_MIN_MEMBERS   2u
#define RT_DTAL_LOD_UID_SEED      0x9E3779B97F4A7C15ull
#define RT_DTAL_LOD_HASH_SEED     14695981039346656037ull

typedef struct
{
    uint32_t level;
    int32_t  cell[3];
    int32_t  axis;
    uint32_t pad;
    uint64_t emissionKey;
    uint32_t styleKey;
    uint32_t passClass;
} rt_dtal_lod_key_t;

typedef struct
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
    uint32_t fill;
    uint32_t assignedCount;
    uint32_t publishedIndex;
    uint32_t publishable;
    uint32_t refused;
    uint32_t dirty;
    uint32_t dirtyMembers;
    double   powerSum;
    double   originSum[3];
    double   normalSum[3];
    double   areaSum;
    float    center[3];
    float    normal[3];
    float    boundsRadius;
} rt_dtal_lod_node_internal_t;

typedef struct
{
    uint64_t uid;
    uint32_t node;
    uint32_t publishedNode;
    uint32_t applied;
    int32_t  band;
    uint32_t visibility;
    uint32_t pendingVisibility;
    uint32_t lastRawVisibility;
    uint32_t hasState;
    uint32_t visitStamp;
    double   pendingSince;
    double   appliedPower;
    double   appliedOriginSum[3];
    double   appliedNormalSum[3];
    double   appliedArea;
} rt_dtal_lod_slot_t;

typedef struct
{
    uint32_t level;
    int32_t  cell[3];
    int32_t  axis;
    uint64_t emissionKey;
    uint32_t styleKey;
    uint32_t passClass;
    uint32_t node;
    uint32_t used;
} rt_dtal_lod_mapentry_t;

struct rt_dtal_lod_s
{
    int                          built;
    rt_dtal_lod_config_t         buildConfig;
    int                          groupCount;

    rt_dtal_lod_slot_t          *slots;
    int                          slotCapacity;

    rt_dtal_lod_node_internal_t *nodes;
    int                          nodeCount;
    int                          nodeCapacity;

    uint32_t                    *members;
    int                          memberCount;
    int                          memberCapacity;

    rt_dtal_lod_mapentry_t      *map;
    int                          mapCapacity;
    int                          mapUsed;

    rt_dtal_lod_node_t          *pubNodes;
    int                          pubNodeCapacity;
    uint32_t                    *pubMembers;
    int                          pubMemberCapacity;
    uint32_t                     pubCount;
    uint32_t                     pubMemberUsed;

    rt_dtal_lod_stats_t          stats;
    uint32_t                     tick;
    int                          tickPubDirty;
};

static int RT_DtalLodReserve(void **data, int *capacity, int needed, size_t elementSize)
{
    if (needed <= *capacity)
        return 1;

    int newCapacity = *capacity > 0 ? *capacity : 64;

    while (newCapacity < needed)
    {
        if (newCapacity > (1 << 29))
            return 0;
        newCapacity *= 2;
    }

    void *grown = realloc(*data, (size_t)newCapacity * elementSize);

    if (grown == NULL)
        return 0;

    memset((char *)grown + (size_t)*capacity * elementSize, 0, (size_t)(newCapacity - *capacity) * elementSize);
    *data = grown;
    *capacity = newCapacity;
    return 1;
}

static uint64_t RT_DtalLodHashBytes(const void *data, size_t size, uint64_t seed)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint64_t             hash = seed;

    for (size_t i = 0; i < size; i++)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }

    return hash;
}

static double RT_DtalLodCellSize(const rt_dtal_lod_config_t *config, int level)
{
    const double radians = (double)config->screenAngleDeg * 3.14159265358979323846 / 180.0;

    return 2.0 * tan(radians * 0.5) * (double)config->startDistance * pow(2.0, (double)level);
}

static void RT_DtalLodCellOf(const float origin[3], double cellSize, int32_t cell[3])
{
    for (int i = 0; i < 3; i++)
    {
        double value = floor((double)origin[i] / cellSize);

        if (!(value > -RT_DTAL_LOD_CELL_LIMIT))
            value = -RT_DTAL_LOD_CELL_LIMIT;
        if (value > RT_DTAL_LOD_CELL_LIMIT)
            value = RT_DTAL_LOD_CELL_LIMIT;

        cell[i] = (int32_t)value;
    }
}

static double RT_DtalLodDistance(const float origin[3], const float camera[3])
{
    const double dx = (double)origin[0] - (double)camera[0];
    const double dy = (double)origin[1] - (double)camera[1];
    const double dz = (double)origin[2] - (double)camera[2];

    return sqrt(dx * dx + dy * dy + dz * dz);
}

static double RT_DtalLodEdge(const rt_dtal_lod_config_t *config, int band)
{
    return (double)config->startDistance * pow(2.0, (double)band);
}

static void RT_DtalLodMakeKey(const rt_dtal_lod_input_t *input, int level, const int32_t cell[3], rt_dtal_lod_key_t *key)
{
    memset(key, 0, sizeof(*key));
    key->level = (uint32_t)level;
    key->cell[0] = cell[0];
    key->cell[1] = cell[1];
    key->cell[2] = cell[2];
    key->axis = input->axis;
    key->emissionKey = input->emissionKey;
    key->styleKey = input->styleKey;
    key->passClass = input->passClass;
}

static int RT_DtalLodKeyEquals(const rt_dtal_lod_key_t *key, const rt_dtal_lod_mapentry_t *entry)
{
    return entry->level == key->level && entry->cell[0] == key->cell[0] && entry->cell[1] == key->cell[1] &&
           entry->cell[2] == key->cell[2] && entry->axis == key->axis && entry->emissionKey == key->emissionKey &&
           entry->styleKey == key->styleKey && entry->passClass == key->passClass;
}

static int RT_DtalLodMapGrow(rt_dtal_lod_t *lod)
{
    const int newCapacity = lod->mapCapacity > 0 ? lod->mapCapacity * 2 : 256;

    rt_dtal_lod_mapentry_t *entries = (rt_dtal_lod_mapentry_t *)calloc((size_t)newCapacity, sizeof(rt_dtal_lod_mapentry_t));

    if (entries == NULL)
        return 0;

    const int mask = newCapacity - 1;

    for (int i = 0; i < lod->mapCapacity; i++)
    {
        rt_dtal_lod_mapentry_t *old = &lod->map[i];

        if (!old->used)
            continue;

        uint64_t hash = RT_DtalLodHashBytes(old, offsetof(rt_dtal_lod_mapentry_t, node), RT_DTAL_LOD_HASH_SEED);
        int probe = (int)(hash & (uint64_t)mask);

        while (entries[probe].used)
            probe = (probe + 1) & mask;

        entries[probe] = *old;
    }

    free(lod->map);
    lod->map = entries;
    lod->mapCapacity = newCapacity;
    return 1;
}

static rt_dtal_lod_mapentry_t *RT_DtalLodMapFindOrInsert(rt_dtal_lod_t *lod, const rt_dtal_lod_key_t *key, int *created)
{
    *created = 0;

    if (lod->mapCapacity == 0 || (lod->mapUsed + 1) * 10 >= lod->mapCapacity * 7)
    {
        if (!RT_DtalLodMapGrow(lod))
            return NULL;
    }

    uint64_t hash = RT_DtalLodHashBytes(key, sizeof(*key), RT_DTAL_LOD_HASH_SEED);
    const int mask = lod->mapCapacity - 1;
    int probe = (int)(hash & (uint64_t)mask);

    for (int i = 0; i < lod->mapCapacity; i++)
    {
        rt_dtal_lod_mapentry_t *entry = &lod->map[probe];

        if (!entry->used)
        {
            entry->level = key->level;
            entry->cell[0] = key->cell[0];
            entry->cell[1] = key->cell[1];
            entry->cell[2] = key->cell[2];
            entry->axis = key->axis;
            entry->emissionKey = key->emissionKey;
            entry->styleKey = key->styleKey;
            entry->passClass = key->passClass;
            entry->node = RT_DTAL_LOD_NO_NODE;
            entry->used = 1;
            lod->mapUsed++;
            *created = 1;
            return entry;
        }

        if (RT_DtalLodKeyEquals(key, entry))
            return entry;

        probe = (probe + 1) & mask;
    }

    return NULL;
}

static rt_dtal_lod_mapentry_t *RT_DtalLodMapLookup(const rt_dtal_lod_t *lod, const rt_dtal_lod_key_t *key)
{
    if (lod->mapCapacity == 0)
        return NULL;

    uint64_t hash = RT_DtalLodHashBytes(key, sizeof(*key), RT_DTAL_LOD_HASH_SEED);
    const int mask = lod->mapCapacity - 1;
    int probe = (int)(hash & (uint64_t)mask);

    for (int i = 0; i < lod->mapCapacity; i++)
    {
        rt_dtal_lod_mapentry_t *entry = &lod->map[probe];

        if (!entry->used)
            return NULL;

        if (RT_DtalLodKeyEquals(key, entry))
            return entry;

        probe = (probe + 1) & mask;
    }

    return NULL;
}

static int RT_DtalLodNodeAdd(rt_dtal_lod_t *lod, const rt_dtal_lod_key_t *key, const rt_dtal_lod_input_t *input, uint32_t *indexOut)
{
    if (!RT_DtalLodReserve((void **)&lod->nodes, &lod->nodeCapacity, lod->nodeCount + 1, sizeof(rt_dtal_lod_node_internal_t)))
        return 0;

    rt_dtal_lod_node_internal_t *node = &lod->nodes[lod->nodeCount];
    memset(node, 0, sizeof(*node));

    node->band = (int32_t)key->level;
    node->cell[0] = key->cell[0];
    node->cell[1] = key->cell[1];
    node->cell[2] = key->cell[2];
    node->axis = input->axis;
    node->emissionKey = input->emissionKey;
    node->styleKey = input->styleKey;
    node->passClass = input->passClass;
    node->publishedIndex = RT_DTAL_LOD_NO_NODE;
    node->uid = RT_DtalLodHashBytes(key, sizeof(*key), RT_DTAL_LOD_UID_SEED);

    if (node->uid == 0)
        node->uid = 1;

    *indexOut = (uint32_t)lod->nodeCount;
    lod->nodeCount++;
    return 1;
}

void RT_Dtal_Lod_Reset(rt_dtal_lod_t *lod)
{
    if (lod == NULL)
        return;

    lod->built = 0;
    lod->groupCount = 0;
    lod->nodeCount = 0;
    lod->memberCount = 0;
    lod->mapUsed = 0;
    lod->pubCount = 0;
    lod->pubMemberUsed = 0;
    lod->tick = 0;
    lod->tickPubDirty = 0;

    memset(&lod->buildConfig, 0, sizeof(lod->buildConfig));
    memset(&lod->stats, 0, sizeof(lod->stats));

    if (lod->map != NULL)
        memset(lod->map, 0, (size_t)lod->mapCapacity * sizeof(rt_dtal_lod_mapentry_t));
    if (lod->slots != NULL)
        memset(lod->slots, 0, (size_t)lod->slotCapacity * sizeof(rt_dtal_lod_slot_t));
}

rt_dtal_lod_t *RT_Dtal_Lod_Create(void)
{
    rt_dtal_lod_t *lod = (rt_dtal_lod_t *)calloc(1, sizeof(rt_dtal_lod_t));

    return lod;
}

void RT_Dtal_Lod_Destroy(rt_dtal_lod_t *lod)
{
    if (lod == NULL)
        return;

    free(lod->slots);
    free(lod->nodes);
    free(lod->members);
    free(lod->map);
    free(lod->pubNodes);
    free(lod->pubMembers);
    free(lod);
}

int RT_Dtal_Lod_Build(rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config, const rt_dtal_lod_input_t *inputs, int count)
{
    if (lod == NULL || config == NULL)
        return 0;
    if (!(config->startDistance > 0.0f) || !(config->screenAngleDeg > 0.0f) || config->screenAngleDeg >= 179.0f)
        return 0;
    if (config->maxBands < 1 || config->maxBands > RT_DTAL_LOD_MAX_BANDS)
        return 0;
    if (count < 0 || (count > 0 && inputs == NULL))
        return 0;

    RT_Dtal_Lod_Reset(lod);

    lod->buildConfig.startDistance = config->startDistance;
    lod->buildConfig.screenAngleDeg = config->screenAngleDeg;
    lod->buildConfig.maxBands = config->maxBands;
    lod->groupCount = count;

    if (!RT_DtalLodReserve((void **)&lod->slots, &lod->slotCapacity, count, sizeof(rt_dtal_lod_slot_t)))
        return 0;

    for (int level = 0; level < config->maxBands; level++)
    {
        const double cellSize = RT_DtalLodCellSize(config, level);

        for (int i = 0; i < count; i++)
        {
            if (inputs[i].flags & RT_DTAL_LOD_INPUT_SINGLETON)
                continue;

            int32_t cell[3];
            RT_DtalLodCellOf(inputs[i].origin, cellSize, cell);

            rt_dtal_lod_key_t key;
            RT_DtalLodMakeKey(&inputs[i], level, cell, &key);

            int                      created = 0;
            rt_dtal_lod_mapentry_t *entry = RT_DtalLodMapFindOrInsert(lod, &key, &created);

            if (entry == NULL)
                return 0;

            if (created)
            {
                uint32_t index = 0;

                if (!RT_DtalLodNodeAdd(lod, &key, &inputs[i], &index))
                    return 0;

                entry->node = index;
            }

            lod->nodes[entry->node].memberCount++;
        }
    }

    int total = 0;

    for (int n = 0; n < lod->nodeCount; n++)
    {
        lod->nodes[n].firstMember = (uint32_t)total;
        lod->nodes[n].fill = 0;
        total += (int)lod->nodes[n].memberCount;
    }

    if (total > 0 && !RT_DtalLodReserve((void **)&lod->members, &lod->memberCapacity, total, sizeof(uint32_t)))
        return 0;

    lod->memberCount = total;

    for (int level = 0; level < config->maxBands; level++)
    {
        const double cellSize = RT_DtalLodCellSize(config, level);

        for (int i = 0; i < count; i++)
        {
            if (inputs[i].flags & RT_DTAL_LOD_INPUT_SINGLETON)
                continue;

            int32_t cell[3];
            RT_DtalLodCellOf(inputs[i].origin, cellSize, cell);

            rt_dtal_lod_key_t key;
            RT_DtalLodMakeKey(&inputs[i], level, cell, &key);

            rt_dtal_lod_mapentry_t *entry = RT_DtalLodMapLookup(lod, &key);

            if (entry == NULL)
                return 0;

            rt_dtal_lod_node_internal_t *node = &lod->nodes[entry->node];

            if (node->fill >= node->memberCount)
                return 0;

            lod->members[node->firstMember + node->fill] = (uint32_t)i;
            node->fill++;
        }
    }

    for (int n = 0; n < lod->nodeCount; n++)
        if (lod->nodes[n].fill != lod->nodes[n].memberCount)
            return 0;

    lod->built = 1;
    lod->stats.groups = (uint64_t)count;
    return 1;
}

static void RT_DtalLodUnapply(rt_dtal_lod_t *lod, rt_dtal_lod_slot_t *slot)
{
    if (!slot->applied)
        return;

    rt_dtal_lod_node_internal_t *node = &lod->nodes[slot->node];

    node->powerSum -= slot->appliedPower;
    node->areaSum -= slot->appliedArea;

    for (int i = 0; i < 3; i++)
    {
        node->originSum[i] -= slot->appliedOriginSum[i];
        node->normalSum[i] -= slot->appliedNormalSum[i];
    }

    if (node->assignedCount > 0)
        node->assignedCount--;

    node->dirty = 1;
    node->dirtyMembers = 1;
    lod->tickPubDirty = 1;

    slot->applied = 0;
    slot->appliedPower = 0.0;
    slot->appliedArea = 0.0;
    memset(slot->appliedOriginSum, 0, sizeof(slot->appliedOriginSum));
    memset(slot->appliedNormalSum, 0, sizeof(slot->appliedNormalSum));
}

static void RT_DtalLodApply(rt_dtal_lod_t *lod, rt_dtal_lod_slot_t *slot, const rt_dtal_lod_input_t *input)
{
    rt_dtal_lod_node_internal_t *node = &lod->nodes[slot->node];
    const double                 power = (double)input->power;

    slot->appliedPower = power;
    slot->appliedArea = (double)input->area;

    for (int i = 0; i < 3; i++)
    {
        slot->appliedOriginSum[i] = power * (double)input->origin[i];
        slot->appliedNormalSum[i] = power * (double)input->normal[i];
    }

    node->powerSum += slot->appliedPower;
    node->areaSum += slot->appliedArea;

    for (int i = 0; i < 3; i++)
    {
        node->originSum[i] += slot->appliedOriginSum[i];
        node->normalSum[i] += slot->appliedNormalSum[i];
    }

    node->assignedCount++;
    node->dirty = 1;
    node->dirtyMembers = 1;
    slot->applied = 1;
    lod->tickPubDirty = 1;
}

static void RT_DtalLodRefresh(rt_dtal_lod_t *lod, rt_dtal_lod_slot_t *slot, const rt_dtal_lod_input_t *input)
{
    rt_dtal_lod_node_internal_t *node = &lod->nodes[slot->node];
    const double                 power = (double)input->power;
    const double                 area = (double)input->area;

    node->powerSum += power - slot->appliedPower;
    node->areaSum += area - slot->appliedArea;

    for (int i = 0; i < 3; i++)
    {
        const double origin = power * (double)input->origin[i];
        const double normal = power * (double)input->normal[i];

        node->originSum[i] += origin - slot->appliedOriginSum[i];
        node->normalSum[i] += normal - slot->appliedNormalSum[i];

        slot->appliedOriginSum[i] = origin;
        slot->appliedNormalSum[i] = normal;
    }

    slot->appliedPower = power;
    slot->appliedArea = area;
    node->dirty = 1;
}

static void RT_DtalLodClearSlot(rt_dtal_lod_t *lod, int group)
{
    rt_dtal_lod_slot_t *slot = &lod->slots[group];

    if (slot->applied)
        RT_DtalLodUnapply(lod, slot);

    memset(slot, 0, sizeof(*slot));
    slot->node = RT_DTAL_LOD_NO_NODE;
    slot->publishedNode = RT_DTAL_LOD_NO_NODE;
    slot->band = -1;
}

static int RT_DtalLodIsPriority(const rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config, const rt_dtal_lod_input_t *inputs,
                                int group, const uint32_t *visibility, const float camera[3])
{
    const rt_dtal_lod_slot_t *slot = &lod->slots[group];
    const uint32_t            raw = visibility != NULL ? visibility[group] : RT_DTAL_LOD_VISIBLE;

    if (raw != slot->lastRawVisibility)
        return 1;

    if (slot->node != RT_DTAL_LOD_NO_NODE)
    {
        const double distance = RT_DtalLodDistance(inputs[group].origin, camera);
        const int    band = slot->band >= 0 ? slot->band : 0;
        const double edge = RT_DtalLodEdge(config, band) * (1.0 - (double)config->hysteresis);

        if (distance < edge)
            return 1;
    }

    return 0;
}

static void RT_DtalLodProcessGroup(rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config, const rt_dtal_lod_input_t *inputs,
                                   int group, const uint32_t *visibility, const float camera[3], double now)
{
    rt_dtal_lod_slot_t       *slot = &lod->slots[group];
    const rt_dtal_lod_input_t *input = &inputs[group];
    const double               distance = RT_DtalLodDistance(input->origin, camera);

    uint32_t raw = visibility != NULL ? visibility[group] : RT_DTAL_LOD_VISIBLE;

    if (raw >= RT_DTAL_LOD_VISIBILITY_COUNT)
        raw = RT_DTAL_LOD_VISIBLE;

    if (!slot->hasState)
    {
        memset(slot, 0, sizeof(*slot));
        slot->uid = input->uid;
        slot->node = RT_DTAL_LOD_NO_NODE;
        slot->publishedNode = RT_DTAL_LOD_NO_NODE;
        slot->band = -1;
        slot->visibility = raw;
        slot->pendingVisibility = raw;
        slot->lastRawVisibility = raw;
        slot->hasState = 1;
    }
    else if (raw != slot->visibility)
    {
        if (raw != slot->pendingVisibility)
        {
            slot->pendingVisibility = raw;
            slot->pendingSince = now;
        }
        else if (now - slot->pendingSince >= (double)config->dwellSeconds)
        {
            slot->visibility = raw;
        }
    }

    slot->lastRawVisibility = raw;

    int distanceBand = -1;

    if (distance >= (double)config->startDistance)
    {
        int band = (int)floor(log2(distance / (double)config->startDistance));

        if (band < 0)
            band = 0;
        if (band > config->maxBands - 1)
            band = config->maxBands - 1;

        distanceBand = band;
    }

    const double hysteresis = (double)config->hysteresis;
    const int    current = slot->band;
    int          next = current;

    if (distanceBand > current)
    {
        if (distance > RT_DtalLodEdge(config, distanceBand) * (1.0 + hysteresis))
            next = distanceBand;
    }
    else if (distanceBand < current)
    {
        if (current >= 0 && distance < RT_DtalLodEdge(config, current) * (1.0 - hysteresis))
            next = distanceBand;
    }

    slot->band = next;

    int effective = next;

    if (effective >= 0 && slot->visibility != RT_DTAL_LOD_VISIBLE)
    {
        effective += config->offscreenShift;

        if (effective < 0)
            effective = 0;
        if (effective > config->maxBands - 1)
            effective = config->maxBands - 1;
    }

    uint32_t desired = RT_DTAL_LOD_NO_NODE;

    if (!(input->flags & RT_DTAL_LOD_INPUT_SINGLETON) && effective >= 0)
    {
        int32_t cell[3];
        RT_DtalLodCellOf(input->origin, RT_DtalLodCellSize(config, effective), cell);

        rt_dtal_lod_key_t key;
        RT_DtalLodMakeKey(input, effective, cell, &key);

        rt_dtal_lod_mapentry_t *entry = RT_DtalLodMapLookup(lod, &key);

        if (entry != NULL)
            desired = entry->node;
    }

    if (desired != slot->node)
    {
        if (slot->applied)
            RT_DtalLodUnapply(lod, slot);

        slot->node = desired;

        if (desired != RT_DTAL_LOD_NO_NODE)
            RT_DtalLodApply(lod, slot, input);
    }
    else if (slot->node != RT_DTAL_LOD_NO_NODE)
    {
        RT_DtalLodRefresh(lod, slot, input);
    }

    slot->visitStamp = lod->tick;
}

static int RT_DtalLodResolve(rt_dtal_lod_t *lod, const rt_dtal_lod_input_t *inputs)
{
    for (int n = 0; n < lod->nodeCount; n++)
    {
        rt_dtal_lod_node_internal_t *node = &lod->nodes[n];

        if (!node->dirty)
            continue;

        node->dirty = 0;

        if (node->assignedCount >= RT_DTAL_LOD_MIN_MEMBERS)
        {
            const double power = node->powerSum;
            const double normalLength = sqrt(node->normalSum[0] * node->normalSum[0] + node->normalSum[1] * node->normalSum[1] +
                                             node->normalSum[2] * node->normalSum[2]);

            if (power > RT_DTAL_LOD_POWER_EPSILON && node->areaSum > RT_DTAL_LOD_AREA_EPSILON &&
                normalLength > RT_DTAL_LOD_NORMAL_REL * power)
            {
                const double invPower = 1.0 / power;
                const double invNormal = 1.0 / normalLength;

                for (int i = 0; i < 3; i++)
                {
                    node->center[i] = (float)(node->originSum[i] * invPower);
                    node->normal[i] = (float)(node->normalSum[i] * invNormal);
                }

                if (node->dirtyMembers)
                {
                    float radius = 0.0f;

                    for (uint32_t m = 0; m < node->memberCount; m++)
                    {
                        const uint32_t      g = lod->members[node->firstMember + m];
                        rt_dtal_lod_slot_t *slot = &lod->slots[g];

                        if (!slot->applied || slot->node != (uint32_t)n)
                            continue;

                        const double dx = (double)inputs[g].origin[0] - (double)node->center[0];
                        const double dy = (double)inputs[g].origin[1] - (double)node->center[1];
                        const double dz = (double)inputs[g].origin[2] - (double)node->center[2];
                        const float  bound = (float)(sqrt(dx * dx + dy * dy + dz * dz) + (double)inputs[g].boundsRadius);

                        if (bound > radius)
                            radius = bound;
                    }

                    node->boundsRadius = radius;
                }

                if (!node->publishable)
                {
                    node->publishable = 1;
                    lod->tickPubDirty = 1;
                }

                node->refused = 0;
            }
            else
            {
                if (!node->refused)
                {
                    node->refused = 1;
                    lod->stats.degenerateRefused++;
                }

                if (node->publishable)
                {
                    node->publishable = 0;
                    lod->tickPubDirty = 1;
                }
            }
        }
        else
        {
            node->refused = 0;

            if (node->publishable)
            {
                node->publishable = 0;
                lod->tickPubDirty = 1;
            }
        }

        node->dirtyMembers = 0;

        if (!lod->tickPubDirty && node->publishable && node->publishedIndex != RT_DTAL_LOD_NO_NODE)
        {
            rt_dtal_lod_node_t *record = &lod->pubNodes[node->publishedIndex];

            for (int i = 0; i < 3; i++)
            {
                record->center[i] = node->center[i];
                record->normal[i] = node->normal[i];
            }

            record->area = (float)node->areaSum;
            record->power = (float)node->powerSum;
            record->boundsRadius = node->boundsRadius;
        }
    }

    if (lod->tickPubDirty)
    {
        lod->pubCount = 0;
        lod->pubMemberUsed = 0;

        for (int n = 0; n < lod->nodeCount; n++)
        {
            rt_dtal_lod_node_internal_t *node = &lod->nodes[n];

            if (!node->publishable)
            {
                node->publishedIndex = RT_DTAL_LOD_NO_NODE;
                continue;
            }

            if (!RT_DtalLodReserve((void **)&lod->pubNodes, &lod->pubNodeCapacity, (int)lod->pubCount + 1, sizeof(rt_dtal_lod_node_t)))
                return 0;

            rt_dtal_lod_node_t *record = &lod->pubNodes[lod->pubCount];
            memset(record, 0, sizeof(*record));

            record->uid = node->uid;
            record->band = node->band;
            record->cell[0] = node->cell[0];
            record->cell[1] = node->cell[1];
            record->cell[2] = node->cell[2];
            record->axis = node->axis;
            record->emissionKey = node->emissionKey;
            record->styleKey = node->styleKey;
            record->passClass = node->passClass;
            record->firstMember = lod->pubMemberUsed;

            uint32_t members = 0;

            for (uint32_t m = 0; m < node->memberCount; m++)
            {
                const uint32_t      g = lod->members[node->firstMember + m];
                rt_dtal_lod_slot_t *slot = &lod->slots[g];

                if (!slot->applied || slot->node != (uint32_t)n)
                    continue;

                if (!RT_DtalLodReserve((void **)&lod->pubMembers, &lod->pubMemberCapacity, (int)lod->pubMemberUsed + 1,
                                       sizeof(uint32_t)))
                    return 0;

                lod->pubMembers[lod->pubMemberUsed] = g;
                lod->pubMemberUsed++;
                members++;
            }

            record->memberCount = members;

            for (int i = 0; i < 3; i++)
            {
                record->center[i] = node->center[i];
                record->normal[i] = node->normal[i];
            }

            record->area = (float)node->areaSum;
            record->power = (float)node->powerSum;
            record->boundsRadius = node->boundsRadius;

            node->publishedIndex = lod->pubCount;
            lod->pubCount++;
        }

        lod->tickPubDirty = 0;
    }

    return 1;
}

int RT_Dtal_Lod_Select(rt_dtal_lod_t *lod, const rt_dtal_lod_config_t *config, const rt_dtal_lod_input_t *inputs, int count,
                       const uint32_t *visibility, const float camera[3], double now, int budget, int *cursor)
{
    if (lod == NULL || config == NULL || camera == NULL)
        return -1;
    if (!lod->built)
        return -1;
    if (count < 0 || (count > 0 && inputs == NULL))
        return -1;
    if (config->startDistance != lod->buildConfig.startDistance || config->screenAngleDeg != lod->buildConfig.screenAngleDeg ||
        config->maxBands != lod->buildConfig.maxBands)
        return -1;

    if (!RT_DtalLodReserve((void **)&lod->slots, &lod->slotCapacity, count, sizeof(rt_dtal_lod_slot_t)))
        return -1;

    lod->tick++;

    if (lod->tick == 0)
    {
        lod->tick = 1;

        for (int g = 0; g < lod->slotCapacity; g++)
            lod->slots[g].visitStamp = 0;
    }

    const uint32_t tick = lod->tick;

    for (int g = 0; g < lod->slotCapacity; g++)
    {
        rt_dtal_lod_slot_t *slot = &lod->slots[g];

        if (!slot->hasState)
            continue;

        if (g >= count || slot->uid != inputs[g].uid)
            RT_DtalLodClearSlot(lod, g);
    }

    const int budgetTotal = budget > 0 ? budget : count;
    int       processed = 0;

    for (int g = 0; g < lod->slotCapacity && processed < budgetTotal; g++)
    {
        rt_dtal_lod_slot_t *slot = &lod->slots[g];

        if (g >= count)
            continue;

        if (!slot->hasState || RT_DtalLodIsPriority(lod, config, inputs, g, visibility, camera))
        {
            RT_DtalLodProcessGroup(lod, config, inputs, g, visibility, camera, now);
            processed++;
        }
    }

    int localCursor = 0;

    if (cursor != NULL)
    {
        localCursor = *cursor;

        if (localCursor < 0 || localCursor >= count)
            localCursor = 0;
    }

    int steps = 0;

    while (processed < budgetTotal && count > 0 && steps < count)
    {
        const int group = localCursor;

        localCursor = (localCursor + 1) % count;
        steps++;

        if (lod->slots[group].hasState && lod->slots[group].visitStamp == tick)
            continue;

        RT_DtalLodProcessGroup(lod, config, inputs, group, visibility, camera, now);
        processed++;
    }

    if (cursor != NULL)
        *cursor = localCursor;

    if (!RT_DtalLodResolve(lod, inputs))
        return -1;

    uint64_t transitions = 0;
    uint64_t merged = 0;
    uint64_t individual = 0;
    uint64_t bandGroups[RT_DTAL_LOD_MAX_BANDS];

    memset(bandGroups, 0, sizeof(bandGroups));

    for (int g = 0; g < count; g++)
    {
        rt_dtal_lod_slot_t *slot = &lod->slots[g];
        uint32_t            published = RT_DTAL_LOD_NO_NODE;

        if (slot->hasState && slot->node != RT_DTAL_LOD_NO_NODE)
        {
            const rt_dtal_lod_node_internal_t *node = &lod->nodes[slot->node];

            if (node->publishable)
                published = node->publishedIndex;
        }

        if (slot->publishedNode != published)
        {
            slot->publishedNode = published;
            transitions++;
        }

        if (published != RT_DTAL_LOD_NO_NODE)
        {
            int band = lod->nodes[slot->node].band;

            if (band < 0)
                band = 0;
            if (band >= RT_DTAL_LOD_MAX_BANDS)
                band = RT_DTAL_LOD_MAX_BANDS - 1;

            merged++;
            bandGroups[band]++;
        }
        else
        {
            individual++;
        }
    }

    lod->stats.groups = (uint64_t)count;
    lod->stats.mergedGroups = merged;
    lod->stats.individualGroups = individual;
    lod->stats.publishedNodes = lod->pubCount;
    lod->stats.transitions = transitions;

    for (int k = 0; k < RT_DTAL_LOD_MAX_BANDS; k++)
        lod->stats.bandGroups[k] = bandGroups[k];

    return (int)transitions;
}

uint32_t RT_Dtal_Lod_NodeCount(const rt_dtal_lod_t *lod)
{
    return lod != NULL ? lod->pubCount : 0;
}

const rt_dtal_lod_node_t *RT_Dtal_Lod_Nodes(const rt_dtal_lod_t *lod)
{
    return lod != NULL ? lod->pubNodes : NULL;
}

const uint32_t *RT_Dtal_Lod_Members(const rt_dtal_lod_t *lod)
{
    return lod != NULL ? lod->pubMembers : NULL;
}

uint32_t RT_Dtal_Lod_GroupNode(const rt_dtal_lod_t *lod, int groupIndex)
{
    if (lod == NULL || groupIndex < 0 || groupIndex >= lod->slotCapacity)
        return RT_DTAL_LOD_NO_NODE;

    if (!lod->slots[groupIndex].hasState)
        return RT_DTAL_LOD_NO_NODE;

    return lod->slots[groupIndex].publishedNode;
}

int RT_Dtal_Lod_GroupCount(const rt_dtal_lod_t *lod)
{
    return lod != NULL ? lod->groupCount : 0;
}

const rt_dtal_lod_stats_t *RT_Dtal_Lod_Stats(const rt_dtal_lod_t *lod)
{
    return lod != NULL ? &lod->stats : NULL;
}
