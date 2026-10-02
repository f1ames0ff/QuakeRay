#include "rt_alias.h"

#include <math.h>
#include <stdlib.h>

int RT_Alias_Build(const double *weights, int count, float *outPrimary, float *outSecondary, uint32_t *outAlias)
{
    if (weights == NULL || outPrimary == NULL || outSecondary == NULL || outAlias == NULL || count <= 0)
        return 0;

    if (count == 1)
    {
        outPrimary[0] = 1.0f;
        outSecondary[0] = 0.0f;
        outAlias[0] = 0u;
        return 1;
    }

    double total = 0.0;

    for (int i = 0; i < count; i++)
    {
        if (isfinite(weights[i]) && weights[i] > 0.0)
            total += weights[i];
    }

    if (!(total > 0.0))
    {
        for (int i = 0; i < count; i++)
        {
            outPrimary[i] = 1.0f;
            outSecondary[i] = 0.0f;
            outAlias[i] = 0u;
        }

        return 1;
    }

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

        const float primary = (float)p;
        outPrimary[i] = primary;
        outSecondary[i] = 1.0f - primary;
    }

    free(scaled);
    free(small);
    free(large);

    return 1;
}
