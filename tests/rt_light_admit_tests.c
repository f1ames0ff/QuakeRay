#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <qray/qray.h>

#include "rt_light_admit.h"

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

static float ColorSum (const float color[3])
{
    float sum = 0.0f;

    for (int i = 0; i < 3; i++)
        sum += (color[i] > 0.0f) ? color[i] : 0.0f;

    return sum;
}

typedef struct registry_s
{
    uint64_t uids[64];
    int      count;
} registry_t;

static void RegistryClear (registry_t *registry)
{
    memset (registry, 0, sizeof (*registry));
}

static void RegistryAdd (registry_t *registry, uint64_t uid)
{
    registry->uids[registry->count++] = uid;
}

static int RegistryHas (const registry_t *registry, uint64_t uid)
{
    for (int i = 0; i < registry->count; i++)
        if (registry->uids[i] == uid)
            return 1;

    return 0;
}

static int RegistriesMatch (const registry_t *renderer, const registry_t *cluster)
{
    if (renderer->count != cluster->count)
        return 0;

    for (int i = 0; i < renderer->count; i++)
        if (!RegistryHas (cluster, renderer->uids[i]))
            return 0;

    return 1;
}

static void ProduceLight (registry_t *renderer, registry_t *cluster, rt_light_budget_t *budget,
                          uint64_t uid, float color[3])
{
    if (!RT_LightBudgetClaim (budget, 1))
        return;

    RT_KeepEmissiveLightColor (color);

    if (ColorSum (color) < RT_LIGHT_RENDERER_KEEP_SUM)
        return;

    RegistryAdd (renderer, uid);
    RegistryAdd (cluster, uid);
}

static void TestBudget (void)
{
    rt_light_budget_t budget;

    CHECK (RT_LIGHT_REGISTRY_CAPACITY == (int) QR_CLUSTER_MAX_REGISTERED_LIGHTS, "the budget matches the renderer's registry capacity");

    RT_LightBudgetInit (&budget);
    CHECK (RT_LightBudgetRemaining (&budget) == RT_LIGHT_REGISTRY_CAPACITY, "a fresh budget holds every light");
    CHECK (RT_LightBudgetClaim (&budget, RT_LIGHT_REGISTRY_CAPACITY) == 1, "the whole budget can be claimed at once");
    CHECK (RT_LightBudgetRemaining (&budget) == 0, "a full budget holds nothing else");
    CHECK (RT_LightBudgetClaim (&budget, 1) == 0, "a light past the budget is refused");
    CHECK (RT_LightBudgetRemaining (&budget) == 0, "a refused claim takes nothing");

    RT_LightBudgetInit (&budget);
    CHECK (RT_LightBudgetClaim (&budget, RT_LIGHT_REGISTRY_CAPACITY + 1) == 0, "an oversized claim is refused");
    CHECK (RT_LightBudgetClaim (&budget, 0) == 0, "an empty claim is refused");
    CHECK (RT_LightBudgetClaim (&budget, -2) == 0, "a negative claim is refused");
    CHECK (RT_LightBudgetClaim (&budget, 7) == 1, "a small claim is taken");
    CHECK (RT_LightBudgetRemaining (&budget) == RT_LIGHT_REGISTRY_CAPACITY - 7, "a taken claim is accounted for");
}

static void TestKeepColor (void)
{
    float visible[3] = { 1.0f, 0.5f, 0.2f };
    float switchedOff[3] = { 0.0f, 0.0f, 0.0f };
    float barelyLit[3] = { 1e-7f, 0.0f, 0.0f };
    float negative[3] = { -1.0f, -2.0f, 0.0f };

    RT_KeepEmissiveLightColor (visible);
    CHECK (visible[0] == 1.0f && visible[1] == 0.5f && visible[2] == 0.2f, "a colour above the threshold is left alone");

    RT_KeepEmissiveLightColor (switchedOff);
    CHECK (ColorSum (switchedOff) >= RT_LIGHT_RENDERER_KEEP_SUM, "a switched-off light keeps an emission above the renderer's threshold");

    RT_KeepEmissiveLightColor (barelyLit);
    CHECK (ColorSum (barelyLit) >= RT_LIGHT_RENDERER_KEEP_SUM, "a barely lit colour is raised");

    RT_KeepEmissiveLightColor (negative);
    CHECK (ColorSum (negative) >= RT_LIGHT_RENDERER_KEEP_SUM, "a colour with no positive channel is raised");
}

static void TestRegistryConsistency (void)
{
    registry_t renderer;
    registry_t cluster;
    rt_light_budget_t budget;

    float visible[3] = { 1.0f, 0.5f, 0.2f };
    float switchedOff[3] = { 0.0f, 0.0f, 0.0f };

    RegistryClear (&renderer);
    RegistryClear (&cluster);
    RT_LightBudgetInit (&budget);

    ProduceLight (&renderer, &cluster, &budget, 1, visible);
    ProduceLight (&renderer, &cluster, &budget, 2, switchedOff);

    CHECK (RegistryHas (&renderer, 2) && RegistryHas (&cluster, 2), "a light its style switched off is in both registries");
    CHECK (RegistriesMatch (&renderer, &cluster), "the registries hold the same lights");

    RegistryClear (&renderer);
    RegistryClear (&cluster);
    RT_LightBudgetInit (&budget);

    ProduceLight (&renderer, &cluster, &budget, 3, visible);
    RegistryAdd (&cluster, 4);

    CHECK (!RegistriesMatch (&renderer, &cluster), "a light the renderer registry has lost is detected");
    CHECK (!RegistryHas (&renderer, 4) && RegistryHas (&cluster, 4), "the cluster registry is the side that holds it");

    RegistryClear (&renderer);
    RegistryClear (&cluster);
    RT_LightBudgetInit (&budget);

    ProduceLight (&renderer, &cluster, &budget, 5, visible);
    RegistryAdd (&renderer, 6);

    CHECK (!RegistriesMatch (&renderer, &cluster), "a light the cluster registry has lost is detected");
    CHECK (RegistryHas (&renderer, 6) && !RegistryHas (&cluster, 6), "the renderer registry is the side that holds it");

    RegistryClear (&renderer);
    RegistryClear (&cluster);
    RT_LightBudgetInit (&budget);

    ProduceLight (&renderer, &cluster, &budget, 7, visible);
    CHECK (RT_LightBudgetClaim (&budget, RT_LightBudgetRemaining (&budget)) == 1, "the budget can be filled to the last light");

    ProduceLight (&renderer, &cluster, &budget, 8, visible);
    CHECK (!RegistryHas (&renderer, 8) && !RegistryHas (&cluster, 8), "a light past the budget exists in neither registry");
    CHECK (RegistriesMatch (&renderer, &cluster), "the budget keeps the registries equal");

    RegistryAdd (&cluster, 9);
    CHECK (!RegistriesMatch (&renderer, &cluster), "a light registered past the budget is detected");
}

int main (void)
{
    TestBudget ();
    TestKeepColor ();
    TestRegistryConsistency ();

    printf ("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures > 0 ? 1 : 0;
}
