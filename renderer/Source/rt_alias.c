#include "rt_alias.h"

#include <math.h>
#include <stdlib.h>

int RT_Alias_Build(const double *weights, int count, float *outPrimary, uint32_t *outAlias)
{
    if (weights == NULL || outPrimary == NULL || outAlias == NULL || count <= 0)
        return 0;

    for (int i = 0; i < count; i++)
    {
        outPrimary[i] = 1.0f;
        outAlias[i] = (uint32_t)i;
    }

    if (count == 1)
    {
        return 1;
    }

    double total = 0.0;

    for (int i = 0; i < count; i++)
    {
        if (isfinite(weights[i]) && weights[i] > 0.0)
            total += weights[i];
    }

    if (!(total > 0.0))
        return 1;

    double *scaled = (double *)malloc(sizeof(double) * (size_t)count);
    int *small = (int *)malloc(sizeof(int) * (size_t)count);
    int *large = (int *)malloc(sizeof(int) * (size_t)count);

    if (scaled == NULL || small == NULL || large == NULL)
    {
        free(scaled);
        free(small);
        free(large);
        return 0;
    }

    int smallCount = 0;
    int largeCount = 0;
    const double scale = (double)count / total;

    for (int i = 0; i < count; i++)
    {
        const double w = (isfinite(weights[i]) && weights[i] > 0.0) ? weights[i] : 0.0;
        const double p = w * scale;
        scaled[i] = p;

        if (p < 1.0)
            small[smallCount++] = i;
        else
            large[largeCount++] = i;
    }

    while (smallCount > 0 && largeCount > 0)
    {
        const int s = small[--smallCount];
        const int l = large[--largeCount];

        outAlias[s] = (uint32_t)l;
        scaled[l] = (scaled[l] + scaled[s]) - 1.0;

        if (scaled[l] < 1.0)
            small[smallCount++] = l;
        else
            large[largeCount++] = l;
    }

    while (largeCount > 0)
    {
        const int l = large[--largeCount];
        scaled[l] = 1.0;
    }

    while (smallCount > 0)
    {
        const int s = small[--smallCount];
        scaled[s] = 1.0;
    }

    for (int i = 0; i < count; i++)
    {
        double p = scaled[i];

        if (!isfinite(p) || p < 0.0)
            p = 0.0;
        if (p > 1.0)
            p = 1.0;

        outPrimary[i] = (float)p;
    }

    free(scaled);
    free(small);
    free(large);

    return 1;
}

void RT_Alias_Marginals(const float *primary, const uint32_t *alias, int count, float *outMarginal)
{
    if (primary == NULL || alias == NULL || outMarginal == NULL || count <= 0)
        return;

    for (int i = 0; i < count; i++)
        outMarginal[i] = primary[i];

    for (int i = 0; i < count; i++)
    {
        const uint32_t chosen = alias[i];

        if (chosen < (uint32_t)count)
        {
            const float secondary = (primary[i] <= 1.0f) ? (1.0f - primary[i]) : 0.0f;

            outMarginal[chosen] += secondary;
        }
    }

    const float inverse = 1.0f / (float)count;

    for (int i = 0; i < count; i++)
    {
        float value = outMarginal[i] * inverse;

        if (!(value >= 0.0f))
            value = 0.0f;

        outMarginal[i] = value;
    }
}
