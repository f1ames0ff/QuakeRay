#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_alias.h"
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

static double AliasPmf(int index, const float *primary, const float *secondary, const uint32_t *alias, int count)
{
    double probability = (double)primary[index];

    for (int i = 0; i < count; i++)
    {
        if ((int)alias[i] == index)
            probability += (double)secondary[i];
    }

    return probability / (double)count;
}

static void TestAliasDistribution(const char *name, const double *weights, int count, double tolerance)
{
    float    *primary = (float *)malloc((size_t)count * sizeof(float));
    float    *secondary = (float *)malloc((size_t)count * sizeof(float));
    uint32_t *alias = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));

    CHECK(primary != NULL && secondary != NULL && alias != NULL, "alias allocation (%s)", name);

    if (primary == NULL || secondary == NULL || alias == NULL)
    {
        free(primary);
        free(secondary);
        free(alias);
        return;
    }

    CHECK(RT_Alias_Build(weights, count, primary, secondary, alias), "alias build (%s)", name);

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
        const double actual = AliasPmf(i, primary, secondary, alias, count);

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
            const double expected = AliasPmf(i, primary, secondary, alias, count);
            const double frequency = (double)hits[i] / (double)samples;
            const double sigma = sqrt(fmax(expected * (1.0 - expected), 1e-12) / (double)samples);

            CHECK(fabs(frequency - expected) <= 5.0 * sigma + 1e-4,
                  "alias sample %s index %d expected %.6f got %.6f", name, i, expected, frequency);
        }

        free(hits);
    }

    free(primary);
    free(secondary);
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
        const double actual0 = AliasPmf(0, build->memberProb, build->memberAliasProb, build->memberAlias, 2);
        const double actual1 = AliasPmf(1, build->memberProb, build->memberAliasProb, build->memberAlias, 2);

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
    TestBuilderBudgetFailure();

    printf("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
