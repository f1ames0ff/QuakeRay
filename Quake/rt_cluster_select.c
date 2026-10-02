#include "rt_cluster_select.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rt_alias.h"

#define RT_CLUSTER_SELECT_MASS_FLOOR 0.001

typedef struct rt_cluster_order_s
{
    float    key;
    uint32_t index;
} rt_cluster_order_t;

static int RT_ClusterSelect_OrderDescending(const void *a, const void *b)
{
    const rt_cluster_order_t *oa = (const rt_cluster_order_t *)a;
    const rt_cluster_order_t *ob = (const rt_cluster_order_t *)b;

    if (oa->key > ob->key)
        return -1;
    if (oa->key < ob->key)
        return 1;
    if (oa->index < ob->index)
        return -1;
    if (oa->index > ob->index)
        return 1;
    return 0;
}

static int RT_ClusterSelect_OrderAscending(const void *a, const void *b)
{
    const rt_cluster_order_t *oa = (const rt_cluster_order_t *)a;
    const rt_cluster_order_t *ob = (const rt_cluster_order_t *)b;

    if (oa->key < ob->key)
        return -1;
    if (oa->key > ob->key)
        return 1;
    if (oa->index < ob->index)
        return -1;
    if (oa->index > ob->index)
        return 1;
    return 0;
}

static float RT_ClusterSelect_Score(const rt_cluster_candidate_t *candidate)
{
    const float power = (isfinite(candidate->power) && candidate->power > 0.0f) ? candidate->power : 0.0f;
    const float floor = 1.0f;
    float       scale = isfinite(candidate->scaleSquared) ? candidate->scaleSquared : floor;

    if (!(scale > floor))
        scale = floor;

    float distance = isfinite(candidate->distanceSquared) ? candidate->distanceSquared : scale;

    if (!(distance > scale))
        distance = scale;

    return power / distance;
}

static float RT_ClusterSelect_MassWithFloor(double mass, double stratumMax)
{
    if (!(stratumMax > 0.0))
        return 1.0f;

    const double floorValue = RT_CLUSTER_SELECT_MASS_FLOOR * stratumMax;
    double       value = isfinite(mass) && mass > 0.0 ? mass : 0.0;

    if (value < floorValue)
        value = floorValue;

    return (float)value;
}

float RT_ClusterSelect_FastProbability(const rt_cluster_select_t *select, const double *fastMasses,
                                       uint32_t fastSlot, float branchFraction, int stratum, int partitions)
{
    if (select == NULL || fastMasses == NULL || fastSlot >= (uint32_t)select->fastCount)
        return 0.0f;

    if (partitions <= 0 || stratum < 0 || stratum >= partitions)
        return 0.0f;

    double stratumMax = 0.0;

    for (int i = stratum; i < select->fastCount; i += partitions)
    {
        if (fastMasses[i] > stratumMax)
            stratumMax = fastMasses[i];
    }

    double massSum = 0.0;

    for (int i = stratum; i < select->fastCount; i += partitions)
    {
        massSum += RT_ClusterSelect_MassWithFloor(fastMasses[i], stratumMax);
    }

    if (!(massSum > 0.0))
        return 0.0f;

    const float selectedMass = RT_ClusterSelect_MassWithFloor(fastMasses[fastSlot], stratumMax);
    const float probability = (float)((double)selectedMass / (massSum * (double)partitions));

    return (isfinite(probability) && probability > 0.0f) ? probability : 0.0f;
}

int RT_ClusterSelect_FastSelect(const rt_cluster_select_t *select, const double *fastMasses, float u0,
                                uint32_t *outSlot, float *outProbability)
{
    if (select == NULL || fastMasses == NULL || outSlot == NULL || outProbability == NULL)
        return 0;

    *outSlot = 0;
    *outProbability = 0.0f;

    if (select->fastCount <= 0)
        return 0;

    const int partitions = (select->fastCount + RT_CLUSTER_FAST_STRATUM - 1) / RT_CLUSTER_FAST_STRATUM;
    float     scaled = u0 * (float)partitions;
    int       stratum = (int)scaled;

    if (stratum < 0)
        stratum = 0;
    if (stratum >= partitions)
        stratum = partitions - 1;

    const float residual = scaled - (float)stratum;

    double stratumMax = 0.0;

    for (int i = stratum; i < select->fastCount; i += partitions)
    {
        if (fastMasses[i] > stratumMax)
            stratumMax = fastMasses[i];
    }

    double massSum = 0.0;

    for (int i = stratum; i < select->fastCount; i += partitions)
        massSum += RT_ClusterSelect_MassWithFloor(fastMasses[i], stratumMax);

    if (!(massSum > 0.0))
        return 0;

    double      target = (double)residual * massSum;
    int         selected = -1;
    const double partitionsScale = (double)partitions;

    for (int i = stratum; i < select->fastCount; i += partitions)
    {
        const double mass = RT_ClusterSelect_MassWithFloor(fastMasses[i], stratumMax);

        target -= mass;

        if (target <= 0.0)
        {
            selected = i;
            break;
        }
    }

    if (selected < 0)
        return 0;

    const double selectedMass = RT_ClusterSelect_MassWithFloor(fastMasses[selected], stratumMax);
    const float  probability = (float)(selectedMass / (massSum * partitionsScale));

    if (!(probability > 0.0f))
        return 0;

    *outSlot = (uint32_t)selected;
    *outProbability = probability;

    return 1;
}

int RT_ClusterSelect_Build(rt_cluster_select_t *select, const rt_cluster_candidate_t *candidates, int count,
                           int repairedRanking)
{
    if (select == NULL)
        return 0;

    memset(select, 0, sizeof(*select));

    if (count <= 0 || candidates == NULL)
        return 1;

    rt_cluster_order_t *order = (rt_cluster_order_t *)malloc((size_t)count * sizeof(rt_cluster_order_t));
    uint8_t            *selected = (uint8_t *)calloc((size_t)count, 1);
    uint32_t           *tail = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));

    select->tailIndex = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    select->tailProb = (float *)malloc((size_t)count * sizeof(float));
    select->tailMarginal = (float *)malloc((size_t)count * sizeof(float));
    select->tailAlias = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));

    if (order == NULL || selected == NULL || tail == NULL || select->tailIndex == NULL ||
        select->tailProb == NULL || select->tailMarginal == NULL || select->tailAlias == NULL)
    {
        free(order);
        free(selected);
        free(tail);
        RT_ClusterSelect_Free(select);
        return 0;
    }

    select->tailCapacity = count;

    if (!repairedRanking)
    {
        for (int i = 0; i < count; i++)
        {
            order[i].key = candidates[i].distanceSquared;
            order[i].index = (uint32_t)i;
        }

        qsort(order, (size_t)count, sizeof(order[0]), RT_ClusterSelect_OrderAscending);
    }
    else
    {
        for (int i = 0; i < count; i++)
        {
            order[i].key = candidates[i].distanceSquared;
            order[i].index = (uint32_t)i;
        }

        qsort(order, (size_t)count, sizeof(order[0]), RT_ClusterSelect_OrderAscending);

        const int reserve = count < RT_CLUSTER_NEAREST_RESERVE ? count : RT_CLUSTER_NEAREST_RESERVE;

        for (int i = 0; i < reserve && select->fastCount < RT_CLUSTER_MAX_FAST; i++)
        {
            selected[order[i].index] = 1;
            select->fastIndex[select->fastCount++] = order[i].index;
        }

        for (int i = 0; i < count; i++)
        {
            order[i].key = RT_ClusterSelect_Score(&candidates[i]);
            order[i].index = (uint32_t)i;
        }

        qsort(order, (size_t)count, sizeof(order[0]), RT_ClusterSelect_OrderDescending);

        for (int i = 0; i < count && select->fastCount < RT_CLUSTER_MAX_FAST; i++)
        {
            if (selected[order[i].index])
                continue;

            selected[order[i].index] = 1;
            select->fastIndex[select->fastCount++] = order[i].index;
        }
    }

    if (!repairedRanking)
    {
        for (int i = 0; i < count && select->fastCount < RT_CLUSTER_MAX_FAST; i++)
        {
            select->fastIndex[select->fastCount++] = order[i].index;
        }
    }

    int tailCount = 0;

    if (repairedRanking)
    {
        for (int i = 0; i < count; i++)
        {
            order[i].key = RT_ClusterSelect_Score(&candidates[i]);
            order[i].index = (uint32_t)i;
        }

        qsort(order, (size_t)count, sizeof(order[0]), RT_ClusterSelect_OrderDescending);

        for (int i = 0; i < count; i++)
        {
            if (!selected[order[i].index])
                tail[tailCount++] = order[i].index;
        }
    }
    else
    {
        for (int i = RT_CLUSTER_MAX_FAST; i < count; i++)
            tail[tailCount++] = order[i].index;
    }

    select->tailCount = tailCount;

    if (tailCount > 0)
    {
        double fastPower = 0.0;
        double tailPower = 0.0;

        for (int i = 0; i < select->fastCount; i++)
        {
            const float power = candidates[select->fastIndex[i]].power;

            if (isfinite(power) && power > 0.0f)
                fastPower += power;
        }

        for (int i = 0; i < tailCount; i++)
        {
            const float power = candidates[tail[i]].power;

            if (isfinite(power) && power > 0.0f)
                tailPower += power;
        }

        if (fastPower > 0.0 || tailPower > 0.0)
        {
            const double beta = tailPower / (fastPower + tailPower);

            select->beta = (float)(beta < 0.1 ? 0.1 : (beta > 0.9 ? 0.9 : beta));
        }
        else
        {
            select->beta = 0.5f;
        }

        double positiveTotal = fastPower + tailPower;
        const double floorWeight = 0.001 * (positiveTotal + 1.0);
        double      *weights = (double *)malloc((size_t)tailCount * sizeof(double));

        if (weights == NULL)
        {
            free(order);
            free(selected);
            free(tail);
            RT_ClusterSelect_Free(select);
            return 0;
        }

        for (int i = 0; i < tailCount; i++)
        {
            const float power = candidates[tail[i]].power;
            const double positive = (isfinite(power) && power > 0.0f) ? power : 0.0;

            weights[i] = positive + floorWeight;
            select->tailIndex[i] = tail[i];
        }

        if (!RT_Alias_Build(weights, tailCount, select->tailProb, select->tailMarginal, select->tailAlias))
        {
            free(weights);
            free(order);
            free(selected);
            free(tail);
            RT_ClusterSelect_Free(select);
            return 0;
        }

        RT_Alias_Marginals(select->tailProb, select->tailAlias, tailCount, select->tailMarginal);

        free(weights);
    }
    else if (select->fastCount > 0)
    {
        select->beta = 0.0f;
    }
    else
    {
        select->beta = 1.0f;
    }

    if (select->fastCount == 0 && select->tailCount > 0)
        select->beta = 1.0f;

    free(order);
    free(selected);
    free(tail);

    return 1;
}

void RT_ClusterSelect_Free(rt_cluster_select_t *select)
{
    if (select == NULL)
        return;

    free(select->tailIndex);
    free(select->tailProb);
    free(select->tailMarginal);
    free(select->tailAlias);

    select->tailIndex = NULL;
    select->tailProb = NULL;
    select->tailMarginal = NULL;
    select->tailAlias = NULL;
    select->tailCount = 0;
    select->tailCapacity = 0;
}
