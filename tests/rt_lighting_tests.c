#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_alias.h"
#include "rt_cluster_select.h"
#include "rt_dtal_groups.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition, ...)                          \
    do                                                 \
    {                                                  \
        g_checks++;                                    \
        if (!(condition))                              \
        {                                              \
            g_failures++;                              \
            printf("FAIL: ");                          \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

static uint32_t g_rng = 0x12345678u;

static uint32_t XorShift32(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static double NextUnit(void)
{
    return (double)(XorShift32() >> 8) * (1.0 / 16777216.0);
}

static double AliasPmf(int index, const float *primary, const uint32_t *alias, int count)
{
    double probability = (double)primary[index];

    for (int i = 0; i < count; i++)
    {
        if ((int)alias[i] == index)
            probability += 1.0 - (double)primary[i];
    }

    return probability / (double)count;
}

static void TestAliasDistribution(const char *name, const double *weights, int count, double tolerance)
{
    float    *primary = (float *)malloc((size_t)count * sizeof(float));
    uint32_t *alias = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));

    CHECK(primary != NULL && alias != NULL, "alias allocation (%s)", name);

    if (primary == NULL || alias == NULL)
    {
        free(primary);
        free(alias);
        return;
    }

    CHECK(RT_Alias_Build(weights, count, primary, alias), "alias build (%s)", name);

    double positiveTotal = 0.0;

    for (int i = 0; i < count; i++)
    {
        const double w = weights[i];

        if (isfinite(w) && w > 0.0)
            positiveTotal += w;
    }

    const int uniform = !(positiveTotal > 0.0);

    double sum = 0.0;

    for (int i = 0; i < count; i++)
    {
        const double w = weights[i];
        const double expected = uniform ? 1.0 / (double)count
                                        : ((isfinite(w) && w > 0.0) ? w : 0.0) / positiveTotal;
        const double actual = AliasPmf(i, primary, alias, count);

        sum += actual;

        CHECK(fabs(actual - expected) <= tolerance + expected * tolerance,
              "alias pmf %s index %d expected %.9f got %.9f", name, i, expected, actual);
    }

    CHECK(fabs(sum - 1.0) <= 1e-5, "alias pmf sum %s: %.9f", name, sum);

    const int samples = 200000;
    int      *hits = (int *)calloc((size_t)count, sizeof(int));

    CHECK(hits != NULL, "alias hits allocation (%s)", name);

    if (hits != NULL)
    {
        for (int s = 0; s < samples; s++)
        {
            const double u = NextUnit();
            int          column = (int)(u * (double)count);
            double       frac = u * (double)count - (double)column;

            if (column < 0)
                column = 0;
            if (column >= count)
                column = count - 1;

            if (frac < 0.0)
                frac = 0.0;
            if (frac >= 1.0)
                frac = nextafter(1.0, 0.0);

            const int chosen = (frac < (double)primary[column]) ? column : (int)alias[column];

            CHECK(chosen >= 0 && chosen < count, "alias draw range %s: %d", name, chosen);
            hits[chosen]++;
        }

        for (int i = 0; i < count; i++)
        {
            const double expected = AliasPmf(i, primary, alias, count);
            const double frequency = (double)hits[i] / (double)samples;
            const double sigma = sqrt(fmax(expected * (1.0 - expected), 1e-12) / (double)samples);

            CHECK(fabs(frequency - expected) <= 5.0 * sigma + 1e-4,
                  "alias sample %s index %d expected %.6f got %.6f", name, i, expected, frequency);
        }

        free(hits);
    }

    free(primary);
    free(alias);
}

static void TestAliasEdgeCases(void)
{
    double single[1] = { 1.0 };
    const double zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const double negatives[4] = { -1.0, 2.0, 0.0, -3.0 };
    const double extremes[4] = { 1e12, 1.0, 1e-6, 0.0 };
    double       uniform[129];

    for (int i = 0; i < 129; i++)
        uniform[i] = 1.0;

    TestAliasDistribution("single", single, 1, 1e-6);
    TestAliasDistribution("zeros", zeros, 8, 1e-5);
    TestAliasDistribution("negatives", negatives, 4, 1e-4);
    TestAliasDistribution("extremes", extremes, 4, 1e-4);
    TestAliasDistribution("uniform129", uniform, 129, 1e-5);

    double *large = (double *)malloc(2048 * sizeof(double));
    double *tiny = (double *)malloc(2048 * sizeof(double));

    for (int i = 0; i < 2048; i++)
    {
        large[i] = (i % 7 == 0) ? 1000.0 : 1.0;
        tiny[i] = 1e-9;
    }

    TestAliasDistribution("large2048", large, 2048, 1e-4);
    TestAliasDistribution("tiny2048", tiny, 2048, 1e-4);

    free(large);
    free(tiny);
}

static void MakeRectangleInput(rt_dtal_input_t *input, double originX, double originY, double originZ, double width,
                               double height, uint64_t emissionKey, float referenceWeight)
{
    memset(input, 0, sizeof(*input));

    input->uid = emissionKey ^ 0x51u;
    input->emissionKey = emissionKey;
    input->styleKey = 1u;
    input->sourceIndex = 0u;
    input->origin[0] = (float)originX;
    input->origin[1] = (float)originY;
    input->origin[2] = (float)originZ;
    input->axisU[0] = (float)width;
    input->axisV[1] = (float)height;
    input->normal[2] = 1.0f;
    input->numVerts = 4;
    input->uv[0][0] = 0.0f; input->uv[0][1] = 0.0f;
    input->uv[1][0] = 1.0f; input->uv[1][1] = 0.0f;
    input->uv[2][0] = 1.0f; input->uv[2][1] = 1.0f;
    input->uv[3][0] = 0.0f; input->uv[3][1] = 1.0f;
    input->area = (float)(width * height);
    input->referenceWeight = referenceWeight;
    input->radiantPower = 1.0f;
}

static void MakeTriangleInput(rt_dtal_input_t *input, double originX, double originY, double originZ, double width,
                              double height, uint64_t emissionKey, float referenceWeight)
{
    MakeRectangleInput(input, originX, originY, originZ, width, height, emissionKey, referenceWeight);

    input->numVerts = 3;
    input->uv[2][0] = 1.0f; input->uv[2][1] = 1.0f;
    input->uv[3][0] = 0.0f; input->uv[3][1] = 0.0f;
    input->area = (float)(0.5 * width * height);
    input->uid ^= 0xABu;
}

static rt_dtal_builder_t *BuildFromInputs(const rt_dtal_input_t *inputs, int count, double spacing, int singleton)
{
    rt_dtal_builder_t *builder = RT_Dtal_BuilderCreate();

    CHECK(builder != NULL, "builder allocation");

    if (builder == NULL)
        return NULL;

    for (int i = 0; i < count; i++)
        CHECK(RT_Dtal_BuilderAddInput(builder, &inputs[i]), "builder input %d", i);

    CHECK(RT_Dtal_BuilderBuild(builder, spacing, singleton), "builder build");

    return builder;
}

static double BuildArea(const rt_dtal_build_t *build)
{
    double area = 0.0;

    for (int i = 0; i < build->memberCount; i++)
        area += (double)build->members[i].area;

    return area;
}

static void TestGroupAreaConservation(void)
{
    rt_dtal_input_t input;
    MakeRectangleInput(&input, 0.0, 0.0, 0.0, 256.0, 256.0, 7u, 1.0f);

    rt_dtal_builder_t *builder = BuildFromInputs(&input, 1, 64.0, 0);

    CHECK(builder != NULL, "conservation builder");

    if (builder != NULL)
    {
        const rt_dtal_build_t *build = RT_Dtal_BuilderResult(builder);

        CHECK(build->groupCount == 16, "conservation group count: %d", build->groupCount);
        CHECK(build->memberCount == 32, "conservation member count: %d", build->memberCount);
        CHECK(fabs(BuildArea(build) - 65536.0) <= 65536.0 * 1e-5, "conservation area: %.6f", BuildArea(build));

        double groupArea = 0.0;

        for (int i = 0; i < build->groupCount; i++)
            groupArea += (double)build->groups[i].area;

        CHECK(fabs(groupArea - 65536.0) <= 65536.0 * 1e-5, "conservation group area: %.6f", groupArea);

        for (int i = 0; i < build->groupCount; i++)
        {
            const rt_dtal_group_t *group = &build->groups[i];
            CHECK(group->memberCount > 0, "group %d has members", i);
            CHECK(group->firstMember >= 0 && group->firstMember + group->memberCount <= build->memberCount,
                  "group %d member range", i);
        }

        RT_Dtal_BuilderDestroy(builder);
    }
}

static void TestGroupMergeAndBuckets(void)
{
    rt_dtal_input_t inputs[4];
    MakeRectangleInput(&inputs[0], 0.0, 0.0, 0.0, 32.0, 32.0, 7u, 1.0f);
    MakeRectangleInput(&inputs[1], 32.0, 0.0, 0.0, 32.0, 32.0, 7u, 1.0f);
    MakeRectangleInput(&inputs[2], 0.0, 32.0, 0.0, 32.0, 32.0, 8u, 1.0f);
    MakeRectangleInput(&inputs[3], 64.0, 0.0, 0.0, 4.0, 4.0, 7u, 0.05f);

    rt_dtal_builder_t *builder = BuildFromInputs(inputs, 4, 512.0, 0);

    CHECK(builder != NULL, "merge builder");

    if (builder != NULL)
    {
        const rt_dtal_build_t *build = RT_Dtal_BuilderResult(builder);

        CHECK(build->groupCount == 2, "merge group count: %d", build->groupCount);
        CHECK(build->memberCount == 8, "merge member count: %d", build->memberCount);

        int members7 = 0;
        int members8 = 0;

        for (int i = 0; i < build->groupCount; i++)
        {
            if (build->groups[i].emissionKey == 7u)
                members7 += build->groups[i].memberCount;
            if (build->groups[i].emissionKey == 8u)
                members8 += build->groups[i].memberCount;
        }

        CHECK(members7 == 6, "merge emission 7 members: %d", members7);
        CHECK(members8 == 2, "merge emission 8 members: %d", members8);

        for (int i = 0; i < build->memberCount; i++)
        {
            CHECK(build->memberProb[i] > 0.0f, "member %d positive support: %.6f", i, (double)build->memberProb[i]);
            CHECK(build->memberProb[i] <= 1.0f, "member %d probability range: %.6f", i, (double)build->memberProb[i]);
            CHECK(build->memberAlias[i] < (uint32_t)build->memberCount, "member %d alias index", i);
        }

        RT_Dtal_BuilderDestroy(builder);
    }
}

static void TestGroupWeightedAlias(void)
{
    rt_dtal_input_t inputs[2];
    MakeTriangleInput(&inputs[0], 0.0, 0.0, 0.0, 16.0, 16.0, 7u, 1.0f);
    MakeTriangleInput(&inputs[1], 16.0, 0.0, 0.0, 16.0, 16.0, 7u, 0.0f);

    rt_dtal_builder_t *builder = BuildFromInputs(inputs, 2, 512.0, 0);

    if (builder != NULL)
    {
        const rt_dtal_build_t *build = RT_Dtal_BuilderResult(builder);

        CHECK(build->groupCount == 1, "weighted group count: %d", build->groupCount);
        CHECK(build->memberCount == 2, "weighted member count: %d", build->memberCount);

        const double expected0 = 1.0 / (1.0 + RT_ALIAS_UNIFORM_PRIOR);
        const double expected1 = RT_ALIAS_UNIFORM_PRIOR / (1.0 + RT_ALIAS_UNIFORM_PRIOR);
        const double actual0 = (double)build->memberMarginal[0];
        const double actual1 = (double)build->memberMarginal[1];

        CHECK(fabs(actual0 - expected0) <= 1e-4, "weighted alias 0: %.6f vs %.6f", actual0, expected0);
        CHECK(fabs(actual1 - expected1) <= 1e-4, "weighted alias 1: %.6f vs %.6f", actual1, expected1);

        RT_Dtal_BuilderDestroy(builder);
    }
}

static void TestGroupDeterminismAndSingleton(void)
{
    rt_dtal_input_t inputs[3];
    MakeRectangleInput(&inputs[0], -200.0, -150.0, 16.0, 100.0, 90.0, 11u, 0.4f);
    MakeRectangleInput(&inputs[1], -100.0, -60.0, 16.0, 80.0, 70.0, 11u, 0.8f);
    MakeRectangleInput(&inputs[2], 50.0, 50.0, 8.0, 10.0, 10.0, 12u, 1.0f);

    rt_dtal_builder_t *first = BuildFromInputs(inputs, 3, 64.0, 0);
    rt_dtal_builder_t *second = BuildFromInputs(inputs, 3, 64.0, 0);

    if (first != NULL && second != NULL)
    {
        const rt_dtal_build_t *a = RT_Dtal_BuilderResult(first);
        const rt_dtal_build_t *b = RT_Dtal_BuilderResult(second);

        CHECK(a->groupCount == b->groupCount, "determinism group count");
        CHECK(a->memberCount == b->memberCount, "determinism member count");

        const int n = a->groupCount < b->groupCount ? a->groupCount : b->groupCount;

        for (int i = 0; i < n; i++)
        {
            CHECK(a->groups[i].uid == b->groups[i].uid, "determinism uid %d", i);
            CHECK(a->groups[i].firstMember == b->groups[i].firstMember, "determinism first member %d", i);
            CHECK(a->groups[i].memberCount == b->groups[i].memberCount, "determinism member count %d", i);
        }

        CHECK(fabs(BuildArea(a) - BuildArea(b)) <= 1e-3, "determinism area");
    }

    rt_dtal_builder_t *singleton = BuildFromInputs(inputs, 3, 64.0, 1);

    if (singleton != NULL && first != NULL)
    {
        const rt_dtal_build_t *s = RT_Dtal_BuilderResult(singleton);
        const rt_dtal_build_t *g = RT_Dtal_BuilderResult(first);

        CHECK(s->groupCount == 3, "singleton group count: %d", s->groupCount);
        CHECK(s->memberCount == 3, "singleton member count: %d", s->memberCount);

        for (int i = 0; i < s->groupCount; i++)
            CHECK(s->groups[i].memberCount == 1, "singleton group %d members: %d", i, s->groups[i].memberCount);

        CHECK(fabs(BuildArea(s) - BuildArea(g)) <= 1e-2, "singleton area %.4f vs grouped %.4f", BuildArea(s), BuildArea(g));
    }

    RT_Dtal_BuilderDestroy(first);
    RT_Dtal_BuilderDestroy(second);
    RT_Dtal_BuilderDestroy(singleton);
}

static void TestGroupNegativeCells(void)
{
    rt_dtal_input_t input;
    MakeRectangleInput(&input, -100.5, -100.5, -100.5, 64.0, 64.0, 21u, 1.0f);

    rt_dtal_builder_t *builder = BuildFromInputs(&input, 1, 64.0, 0);

    if (builder != NULL)
    {
        const rt_dtal_build_t *build = RT_Dtal_BuilderResult(builder);

        CHECK(fabs(BuildArea(build) - 4096.0) <= 0.01, "negative cell area: %.6f", BuildArea(build));
        CHECK(build->memberCount >= 4, "negative cell members: %d", build->memberCount);

        int negativeCells = 0;

        for (int i = 0; i < build->groupCount; i++)
        {
            if (build->groups[i].cell[0] < 0 || build->groups[i].cell[1] < 0 || build->groups[i].cell[2] < 0)
                negativeCells++;
        }

        CHECK(negativeCells > 0, "negative cell coordinates present: %d", negativeCells);

        RT_Dtal_BuilderDestroy(builder);
    }
}

static void TestGroupBoundaryOwnership(void)
{
    rt_dtal_input_t left;
    rt_dtal_input_t right;
    MakeRectangleInput(&left, 0.0, 0.0, 0.0, 64.0, 64.0, 31u, 1.0f);
    MakeRectangleInput(&right, 64.0, 0.0, 0.0, 64.0, 64.0, 31u, 1.0f);

    rt_dtal_builder_t *builder = BuildFromInputs(&left, 1, 64.0, 0);
    rt_dtal_builder_t *builder2 = BuildFromInputs(&right, 1, 64.0, 0);

    if (builder != NULL && builder2 != NULL)
    {
        const rt_dtal_build_t *a = RT_Dtal_BuilderResult(builder);
        const rt_dtal_build_t *b = RT_Dtal_BuilderResult(builder2);

        CHECK(a->groupCount == 1, "boundary left groups: %d", a->groupCount);
        CHECK(b->groupCount == 1, "boundary right groups: %d", b->groupCount);

        if (a->groupCount == 1 && b->groupCount == 1)
        {
            CHECK(a->groups[0].cell[0] == 0, "boundary left cell: %d", a->groups[0].cell[0]);
            CHECK(b->groups[0].cell[0] == 1, "boundary right cell: %d", b->groups[0].cell[0]);
        }

        CHECK(fabs(BuildArea(a) - 4096.0) <= 0.01, "boundary left area: %.6f", BuildArea(a));
        CHECK(fabs(BuildArea(b) - 4096.0) <= 0.01, "boundary right area: %.6f", BuildArea(b));
    }

    RT_Dtal_BuilderDestroy(builder);
    RT_Dtal_BuilderDestroy(builder2);
}

#define DTAL_TEST_CAP (4.0 * 3.14159265358979323846)

static double PatchGeometryFactor(double h, double x, double y)
{
    const double dist2 = x * x + y * y + h * h;
    const double dist = sqrt(dist2);

    return h / (dist * dist * dist);
}

static double QuadratureIntegral(double size, double h, int fine)
{
    const double cell = size / (double)fine;
    double       sum = 0.0;

    for (int iy = 0; iy < fine; iy++)
    {
        for (int ix = 0; ix < fine; ix++)
        {
            const double x = -0.5 * size + (ix + 0.5) * cell;
            const double y = -0.5 * size + (iy + 0.5) * cell;

            sum += cell * cell * PatchGeometryFactor(h, x, y);
        }
    }

    return sum;
}

static double CappedExpectation(double size, double h, int fine, int subdivisions)
{
    const double patchSize = size / (double)subdivisions;
    const double patchArea = patchSize * patchSize;
    const double cell = patchSize / (double)fine;
    double       total = 0.0;

    for (int py = 0; py < subdivisions; py++)
    {
        for (int px = 0; px < subdivisions; px++)
        {
            const double patchX = -0.5 * size + (px + 0.5) * patchSize;
            const double patchY = -0.5 * size + (py + 0.5) * patchSize;
            double       patchSum = 0.0;

            for (int iy = 0; iy < fine; iy++)
            {
                for (int ix = 0; ix < fine; ix++)
                {
                    const double x = patchX - 0.5 * patchSize + (ix + 0.5) * cell;
                    const double y = patchY - 0.5 * patchSize + (iy + 0.5) * cell;
                    double       dw = patchArea * PatchGeometryFactor(h, x, y);

                    if (dw > DTAL_TEST_CAP)
                        dw = DTAL_TEST_CAP;

                    patchSum += cell * cell * dw / patchArea;
                }
            }

            total += patchSum;
        }
    }

    return total;
}

static double UncappedExpectation(double size, double h, int fine, int subdivisions)
{
    const double patchSize = size / (double)subdivisions;
    const double cell = patchSize / (double)fine;
    double       total = 0.0;

    for (int py = 0; py < subdivisions; py++)
    {
        for (int px = 0; px < subdivisions; px++)
        {
            const double patchX = -0.5 * size + (px + 0.5) * patchSize;
            const double patchY = -0.5 * size + (py + 0.5) * patchSize;
            double       patchSum = 0.0;

            for (int iy = 0; iy < fine; iy++)
            {
                for (int ix = 0; ix < fine; ix++)
                {
                    const double x = patchX - 0.5 * patchSize + (ix + 0.5) * cell;
                    const double y = patchY - 0.5 * patchSize + (iy + 0.5) * cell;

                    patchSum += cell * cell * PatchGeometryFactor(h, x, y);
                }
            }

            total += patchSum;
        }
    }

    return total;
}

static void TestEstimatorSubdivision(void)
{
    const double size = 64.0;
    const double nearHeight = 8.0;
    const double farHeight = 512.0;
    const int    wholeFine = 64;
    const int    splitParts = 4;
    const int    splitFine = 16;

    const double integralNear = QuadratureIntegral(size, nearHeight, wholeFine);
    const double cappedWholeNear = CappedExpectation(size, nearHeight, wholeFine, 1);
    const double cappedSplitNear = CappedExpectation(size, nearHeight, splitFine, splitParts);
    const double uncappedWholeNear = UncappedExpectation(size, nearHeight, wholeFine, 1);
    const double uncappedSplitNear = UncappedExpectation(size, nearHeight, splitFine, splitParts);

    CHECK(fabs(cappedWholeNear - integralNear) > integralNear * 0.1,
          "old estimator near field is capped: capped %.6f integral %.6f", cappedWholeNear, integralNear);
    CHECK(fabs(cappedSplitNear - integralNear) < fabs(cappedWholeNear - integralNear) * 0.5,
          "old estimator near field depends on the subdivision: whole %.6f split %.6f integral %.6f",
          cappedWholeNear, cappedSplitNear, integralNear);
    CHECK(fabs(uncappedWholeNear - uncappedSplitNear) <= integralNear * 1e-9,
          "new estimator is subdivision invariant: whole %.9f split %.9f", uncappedWholeNear, uncappedSplitNear);

    const double integralFar = QuadratureIntegral(size, farHeight, wholeFine);
    const double cappedWholeFar = CappedExpectation(size, farHeight, wholeFine, 1);
    const double cappedSplitFar = CappedExpectation(size, farHeight, splitFine, splitParts);

    CHECK(fabs(cappedWholeFar - integralFar) <= integralFar * 1e-9,
          "old estimator far field is unbiased: capped %.9f integral %.9f", cappedWholeFar, integralFar);
    CHECK(fabs(cappedSplitFar - integralFar) <= integralFar * 1e-9,
          "old estimator far field split is unbiased: capped %.9f integral %.9f", cappedSplitFar, integralFar);
}

static double SampleTriangleHeight(double u1, double u2, int truncated)
{
    if (truncated)
    {
        u1 *= 0.99;
        u2 *= 0.99;
    }

    const double beta = 1.0 - sqrt(u1);
    const double gamma = (1.0 - beta) * u2;

    return 1.0 - beta - gamma;
}

static void TestEstimatorSamplingDomain(void)
{
    const int    samples = 2000000;
    const double fullMean = 1.0 / 3.0;

    double truncatedSum = 0.0;
    double fullSum = 0.0;

    for (int i = 0; i < samples; i++)
    {
        const double u1 = NextUnit();
        const double u2 = NextUnit();

        truncatedSum += SampleTriangleHeight(u1, u2, 1);
        fullSum += SampleTriangleHeight(u1, u2, 0);
    }

    const double truncatedMean = truncatedSum / (double)samples;
    const double fullMeanEstimate = fullSum / (double)samples;

    CHECK(fabs(truncatedMean - fullMean) > 1e-4,
          "truncated triangle domain shows a bias: %.6f vs %.6f", truncatedMean, fullMean);
    CHECK(fabs(fullMeanEstimate - fullMean) <= 0.002,
          "full triangle domain is unbiased: %.6f vs %.6f", fullMeanEstimate, fullMean);
}

static int CompareUint32(const void *a, const void *b)
{
    const uint32_t va = *(const uint32_t *)a;
    const uint32_t vb = *(const uint32_t *)b;

    return va < vb ? -1 : (va > vb ? 1 : 0);
}

static void TestClusterSelectionCounts(void)
{
    const int counts[] = { 0, 1, 128, 129, 512, 2048 };
    const int samples = 2000000;

    for (int ci = 0; ci < (int)(sizeof(counts) / sizeof(counts[0])); ci++)
    {
        const int count = counts[ci];

        rt_cluster_candidate_t *candidates = (rt_cluster_candidate_t *)calloc((size_t)(count > 0 ? count : 1),
                                                                               sizeof(rt_cluster_candidate_t));
        double *masses = (double *)calloc((size_t)(count > 0 ? count : 1), sizeof(double));

        CHECK(candidates != NULL && masses != NULL, "cluster selection allocation %d", count);

        for (int i = 0; i < count; i++)
        {
            candidates[i].uid = (uint64_t)(i + 1);
            candidates[i].power = (i % 7 == 0) ? 10.0f : ((i % 23 == 0) ? 0.0f : 1.0f);
            candidates[i].distanceSquared = 1.0f + (float)((i * 37) % 500);
            candidates[i].scaleSquared = 1.0f;
            masses[i] = (i % 17 == 0) ? 0.0 : 0.5 + 0.5 * (double)((i * 13) % 11);
        }

        rt_cluster_select_t select;
        CHECK(RT_ClusterSelect_Build(&select, candidates, count, 1), "cluster selection build %d", count);

        CHECK(select.fastCount <= RT_CLUSTER_MAX_FAST, "fast count bound %d", count);
        CHECK(select.fastCount + select.tailCount == count, "partition covers %d: %d + %d",
              count, select.fastCount, select.tailCount);
        CHECK(select.beta >= 0.0f && select.beta <= 1.0f, "beta range %d: %.6f", count, (double)select.beta);

        if (count > 0)
        {
            for (int i = 0; i < select.tailCount; i++)
            {
                CHECK(select.tailProb[i] > 0.0f, "tail support %d index %d: %.9f", count, i, (double)select.tailProb[i]);
                CHECK(select.tailAlias[i] < (uint32_t)select.tailCount, "tail alias range %d", count);
            }
        }

        if (count > 0)
        {
            double sum = 0.0;
            double squareSum = 0.0;
            double pAccum = 0.0;
            int    selectedCount = 0;
            int    tailDraws = 0;
            int    fastDraws = 0;

            for (int s = 0; s < samples; s++)
            {
                const float u0 = (float)NextUnit();
                const float uBranch = (float)NextUnit();
                const float uTail = (float)NextUnit();

                uint32_t chosen = 0;
                float    probability = 0.0f;

                int useTail = 0;

                if (select.tailCount <= 0)
                    useTail = 0;
                else if (select.fastCount <= 0)
                    useTail = 1;
                else
                    useTail = (uBranch < select.beta) ? 1 : 0;

                if (useTail)
                {
                    int column = (int)(uTail * (float)select.tailCount);

                    if (column < 0)
                        column = 0;
                    if (column >= select.tailCount)
                        column = select.tailCount - 1;

                    const float fraction = uTail * (float)select.tailCount - (float)column;
                    const int   tailed = (fraction < select.tailProb[column]) ? column : (int)select.tailAlias[column];
                    const float memberProbability = select.tailMarginal[tailed];

                    probability = select.beta * memberProbability;
                    chosen = select.tailIndex[tailed];
                }
                else
                {
                    uint32_t slot = 0;
                    float    fastProbability = 0.0f;

                    if (RT_ClusterSelect_FastSelect(&select, masses, u0, &slot, &fastProbability))
                    {
                        probability = (1.0f - select.beta) * fastProbability;
                        chosen = select.fastIndex[slot];
                    }
                }

                pAccum += probability;
                tailDraws += useTail;
                fastDraws += useTail ? 0 : 1;

                if (probability > 0.0f && chosen < (uint32_t)count)
                {
                    const double estimator = 1.0 / (double)probability;

                    sum += estimator;
                    squareSum += estimator * estimator;
                    selectedCount++;
                }
            }

            (void)pAccum;
            (void)tailDraws;
            (void)fastDraws;

            const double mean = sum / (double)samples;
            const double expected = (double)count;
            const double variance = squareSum / (double)samples - mean * mean;
            const double standardError = sqrt(fmax(variance, 0.0) / (double)samples);

            CHECK(selectedCount > 0, "cluster selection draws %d", count);
            CHECK(4.0 * standardError <= expected * 0.05, "cluster selection decides %d: se %.6f", count, standardError);
            CHECK(fabs(mean - expected) <= 4.0 * standardError + expected * 1e-4,
                  "cluster selection mean %d: %.6f vs %.6f (se %.6f)", count, mean, expected, standardError);
        }

        RT_ClusterSelect_Free(&select);
        free(candidates);
        free(masses);
    }
}

static void TestClusterSelectionOrderIdentity(void)
{
    const int count = 512;

    rt_cluster_candidate_t *candidates = (rt_cluster_candidate_t *)calloc((size_t)count, sizeof(rt_cluster_candidate_t));
    rt_cluster_candidate_t *shuffled = (rt_cluster_candidate_t *)calloc((size_t)count, sizeof(rt_cluster_candidate_t));

    CHECK(candidates != NULL && shuffled != NULL, "order identity allocation");

    for (int i = 0; i < count; i++)
    {
        candidates[i].uid = (uint64_t)(i + 1);
        candidates[i].power = (i % 7 == 0) ? 10.0f : 1.0f;
        candidates[i].distanceSquared = 1.0f + (float)((i * 37) % 500);
        candidates[i].scaleSquared = 1.0f;
        shuffled[i] = candidates[i];
    }

    for (int i = count - 1; i > 0; i--)
    {
        const int j = (int)(NextUnit() * (double)(i + 1));
        const rt_cluster_candidate_t tmp = shuffled[i];

        shuffled[i] = shuffled[j];
        shuffled[j] = tmp;
    }

    rt_cluster_select_t first;
    rt_cluster_select_t second;

    CHECK(RT_ClusterSelect_Build(&first, candidates, count, 1), "order identity first");
    CHECK(RT_ClusterSelect_Build(&second, shuffled, count, 1), "order identity second");

    CHECK(first.fastCount == second.fastCount, "order identity fast count: %d vs %d", first.fastCount, second.fastCount);
    CHECK(first.tailCount == second.tailCount, "order identity tail count: %d vs %d", first.tailCount, second.tailCount);

    uint32_t firstFast[RT_CLUSTER_MAX_FAST];
    uint32_t secondFast[RT_CLUSTER_MAX_FAST];
    uint32_t firstTail[1024];
    uint32_t secondTail[1024];

    CHECK(first.tailCount <= (int)(sizeof(firstTail) / sizeof(firstTail[0])), "order identity tail capacity");

    for (int i = 0; i < first.fastCount; i++)
    {
        firstFast[i] = (uint32_t)candidates[first.fastIndex[i]].uid;
        secondFast[i] = (uint32_t)shuffled[second.fastIndex[i]].uid;
    }

    for (int i = 0; i < first.tailCount && i < (int)(sizeof(firstTail) / sizeof(firstTail[0])); i++)
    {
        firstTail[i] = (uint32_t)candidates[first.tailIndex[i]].uid;
        secondTail[i] = (uint32_t)shuffled[second.tailIndex[i]].uid;
    }

    qsort(firstFast, (size_t)first.fastCount, sizeof(uint32_t), CompareUint32);
    qsort(secondFast, (size_t)second.fastCount, sizeof(uint32_t), CompareUint32);
    qsort(firstTail, (size_t)first.tailCount, sizeof(uint32_t), CompareUint32);
    qsort(secondTail, (size_t)second.tailCount, sizeof(uint32_t), CompareUint32);

    for (int i = 0; i < first.fastCount; i++)
        CHECK(firstFast[i] == secondFast[i], "order identity fast member %d", i);

    for (int i = 0; i < first.tailCount; i++)
        CHECK(firstTail[i] == secondTail[i], "order identity tail member %d", i);

    RT_ClusterSelect_Free(&first);
    RT_ClusterSelect_Free(&second);
    free(candidates);
    free(shuffled);
}

static void TestBuilderBudgetFailure(void)
{
    rt_dtal_builder_t *builder = RT_Dtal_BuilderCreate();

    CHECK(builder != NULL, "budget builder");

    if (builder != NULL)
    {
        CHECK(!RT_Dtal_BuilderBuild(builder, 0.0, 0), "zero spacing refused");
        CHECK(!RT_Dtal_BuilderBuild(builder, -8.0, 0), "negative spacing refused");
        CHECK(!RT_Dtal_BuilderBuild(builder, NAN, 0), "nan spacing refused");

        RT_Dtal_BuilderDestroy(builder);
    }
}

int main(void)
{
    TestAliasEdgeCases();
    TestGroupAreaConservation();
    TestGroupMergeAndBuckets();
    TestGroupWeightedAlias();
    TestGroupDeterminismAndSingleton();
    TestGroupNegativeCells();
    TestGroupBoundaryOwnership();
    TestEstimatorSubdivision();
    TestEstimatorSamplingDomain();
    TestClusterSelectionCounts();
    TestClusterSelectionOrderIdentity();
    TestBuilderBudgetFailure();

    printf("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
