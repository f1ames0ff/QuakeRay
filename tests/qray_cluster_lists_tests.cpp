#include "ClusterLightLists.h"
#include "LightManager.h"
#include "WorldLights.h"
#include "UserFunction.h"

#include "Generated/ShaderCommonC.h"
#include "rt_light_admit.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition, ...)                          \
    do                                                 \
    {                                                  \
        g_checks++;                                    \
        if (!(condition))                              \
        {                                              \
            g_failures++;                              \
            std::printf("FAIL: ");                     \
            std::printf(__VA_ARGS__);                  \
            std::printf("\n");                         \
        }                                              \
    } while (0)

constexpr uint32_t kSmallClusters = 8;
constexpr uint8_t  kSmallRowByte = 0x7E;
constexpr float    kClusterSize = 8.0f;
constexpr uint64_t kHole = ~0ull;

struct PrintState
{
    int problems = 0;
};

void CapturePrint(const char *pMessage, void *pUserData)
{
    PrintState *pState = static_cast<PrintState *>(pUserData);

    if (std::strstr(pMessage, "mismatch") != nullptr || std::strstr(pMessage, "do not match") != nullptr)
    {
        pState->problems++;
    }
}

struct SyntheticMap
{
    std::vector<QrFloat3D> mins;
    std::vector<QrFloat3D> maxs;
    std::vector<uint8_t> flags;
    std::vector<int32_t> visOffsets;
    std::vector<uint8_t> vis;
    uint32_t rowBytes = 0;

    qray::WorldLights world;

    SyntheticMap(uint32_t clusters, uint32_t rowBytesIn, uint8_t rowByte, uint32_t generation,
                 bool zeroRuns = false)
    {
        rowBytes = rowBytesIn;

        mins.resize(clusters);
        maxs.resize(clusters);
        flags.assign(clusters, 0);
        visOffsets.resize(clusters);
        vis.assign(size_t(clusters) * rowBytes, rowByte);

        if (zeroRuns)
        {
            for (size_t i = 0; i + 1 < vis.size(); i += 2)
            {
                vis[i] = 0x00;
                vis[i + 1] = 0xFF;
            }
        }

        for (uint32_t c = 0; c < clusters; c++)
        {
            const float x = float(c) * kClusterSize;

            mins[c].data[0] = x;
            mins[c].data[1] = 0.0f;
            mins[c].data[2] = 0.0f;
            maxs[c].data[0] = x + kClusterSize;
            maxs[c].data[1] = kClusterSize;
            maxs[c].data[2] = kClusterSize;

            visOffsets[c] = int32_t(size_t(c) * rowBytes);
        }

        QrWorldLightsUploadInfo upload = {};
        upload.numClusters = clusters;
        upload.pClusterMins = mins.data();
        upload.pClusterMaxs = maxs.data();
        upload.pClusterFlags = flags.data();
        upload.pVisOffsets = visOffsets.data();
        upload.pVisData = vis.data();
        upload.visDataSize = uint32_t(vis.size());
        upload.pvsRowBytes = rowBytes;
        upload.clusterGeneration = generation;

        world.Upload(upload, nullptr);
    }
};

struct TestLight
{
    uint64_t uid;
    uint32_t cluster;
    float    color[3] = { 1.0f, 0.5f, 0.2f };
};

using Slots = std::vector<std::vector<uint64_t>>;

Slots Snapshot(const qray::ClusterLightLists &lists, uint32_t clusters)
{
    Slots snapshot(clusters);

    for (uint32_t c = 0; c < clusters; c++)
    {
        uint64_t uids[Q2_LIGHT_LIST_MAX_PER_CELL];
        uint32_t count = 0;

        lists.GetClusterSlots(c, uids, Q2_LIGHT_LIST_MAX_PER_CELL, &count);

        snapshot[c].assign(uids, uids + count);
    }

    return snapshot;
}

std::vector<uint64_t> NamedUids(const qray::ClusterLightLists &lists, uint32_t clusters)
{
    std::vector<uint64_t> uids;

    for (uint32_t c = 0; c < clusters; c++)
    {
        uint64_t list[Q2_LIGHT_LIST_MAX_PER_CELL];
        uint32_t count = 0;

        lists.GetClusterList(c, list, Q2_LIGHT_LIST_MAX_PER_CELL, &count);

        for (uint32_t i = 0; i < count; i++)
        {
            if (std::find(uids.begin(), uids.end(), list[i]) == uids.end())
            {
                uids.push_back(list[i]);
            }
        }

        uint64_t tailUids[256];
        float    prob[256];
        float    marginal[256];
        uint32_t alias[256];
        float    beta = 0.0f;

        lists.GetClusterTail(c, tailUids, prob, marginal, alias, &beta, 256, &count);

        for (uint32_t i = 0; i < count; i++)
        {
            if (std::find(uids.begin(), uids.end(), tailUids[i]) == uids.end())
            {
                uids.push_back(tailUids[i]);
            }
        }
    }

    return uids;
}

std::vector<uint64_t> AdmittedUids(const std::vector<TestLight> &lights, bool keepColors)
{
    std::vector<uint64_t> admitted;

    for (const TestLight &light : lights)
    {
        float color[3] = { light.color[0], light.color[1], light.color[2] };

        if (keepColors)
        {
            RT_KeepEmissiveLightColor(color);
        }

        float sum = 0.0f;

        for (int i = 0; i < 3; i++)
        {
            sum += (color[i] > 0.0f) ? color[i] : 0.0f;
        }

        if (sum >= RT_LIGHT_RENDERER_KEEP_SUM)
        {
            admitted.push_back(light.uid);
        }
    }

    return admitted;
}

bool RegistriesMatch(const std::vector<uint64_t> &renderer, const std::vector<uint64_t> &cluster)
{
    for (uint64_t uid : renderer)
    {
        if (std::find(cluster.begin(), cluster.end(), uid) == cluster.end())
        {
            return false;
        }
    }

    for (uint64_t uid : cluster)
    {
        if (std::find(renderer.begin(), renderer.end(), uid) == renderer.end())
        {
            return false;
        }
    }

    return true;
}

void FeedFrame(qray::ClusterLightLists &lists, const qray::WorldLights &world,
               const std::vector<TestLight> &lights, uint32_t &frameIndex, qray::UserPrint *pPrint,
               float topUpReach, int32_t allowOverflow, int32_t validate)
{
    std::vector<QrClusterLightSource> sources(lights.size());

    for (size_t i = 0; i < lights.size(); i++)
    {
        sources[i].uniqueID = lights[i].uid;
        sources[i].origin.data[0] = 1000.0f;
        sources[i].origin.data[1] = 1000.0f;
        sources[i].origin.data[2] = 1000.0f;
        sources[i].cluster = lights[i].cluster;
        sources[i].reach = 0.0f;
        sources[i].radius = 0.0f;
        sources[i].clusterCount = 0;
        sources[i].pClusters = nullptr;
        sources[i].power = 1.0f;
    }

    QrClusterLightSourcesUploadInfo upload = {};
    upload.numLights = uint32_t(sources.size());
    upload.pLights = sources.data();
    upload.topUpReach = topUpReach;
    upload.allowIncremental = 1;
    upload.allowOverflow = allowOverflow;
    upload.validate = validate;

    lists.SetSources(world, upload, nullptr, pPrint, frameIndex++);
}

void TestReuseAndEdit(const SyntheticMap &map)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    const std::vector<TestLight> first = { { 1001, 3 }, { 1002, 5 } };
    const std::vector<TestLight> withNew = { { 1001, 3 }, { 1002, 5 }, { 1003, 6 } };

    FeedFrame(lists, map.world, first, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 1, "the first frame composes the lists");

    const Slots composed = Snapshot(lists, kSmallClusters);

    FeedFrame(lists, map.world, first, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 0 && lists.GetStats().reusedFrames == 1, "a frame of the same set reuses the lists");
    CHECK(Snapshot(lists, kSmallClusters) == composed, "a reused frame publishes the same slots");

    FeedFrame(lists, map.world, withNew, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 0, "a light that appears does not compose");
    CHECK(lists.GetStats().reusedFrames == 1, "a light that appears is patched into the lists");

    const Slots added = Snapshot(lists, kSmallClusters);
    bool edgesGrew = false;

    for (uint32_t c = 0; c < kSmallClusters; c++)
    {
        CHECK(added[c].size() >= composed[c].size() && added[c].size() <= composed[c].size() + 1,
              "cluster %u takes in at most the one new light", c);
        CHECK(added[c].size() >= composed[c].size() &&
                  std::equal(composed[c].begin(), composed[c].end(), added[c].begin()),
              "the lights of cluster %u stay in their slots", c);

        edgesGrew = edgesGrew || added[c].size() != composed[c].size();
    }

    CHECK(edgesGrew, "the new light reached the clusters its row names");

    FeedFrame(lists, map.world, first, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 0, "a light that leaves does not compose");
    CHECK(Snapshot(lists, kSmallClusters) == composed, "the lists come back exactly as they were");

    CHECK(print.problems == 0, "the composition validation reports no mismatch");
}

void TestRemovalRun(const SyntheticMap &map)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    std::vector<TestLight> lights;

    for (uint32_t i = 0; i < 40; i++)
    {
        lights.push_back({ 2000 + i, 2 + (i % 6) });
    }

    FeedFrame(lists, map.world, lights, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 1, "the first frame composes the lists");

    Slots expected = Snapshot(lists, kSmallClusters);

    for (uint32_t step = 0; step < 30; step++)
    {
        const uint64_t removed = lights.front().uid;

        lights.erase(lights.begin());
        FeedFrame(lists, map.world, lights, frameIndex, &printer, 0.0f, 0, 1);

        CHECK(lists.GetStats().composedFrames == 0, "removal %u leaves the scene composed once", step);
        CHECK(lists.GetStats().reusedFrames == 1, "removal %u is patched into the lists", step);

        for (uint32_t c = 0; c < kSmallClusters; c++)
        {
            for (uint64_t &uid : expected[c])
            {
                if (uid == removed)
                {
                    uid = kHole;
                }
            }

            while (!expected[c].empty() && expected[c].back() == kHole)
            {
                expected[c].pop_back();
            }
        }

        CHECK(Snapshot(lists, kSmallClusters) == expected, "removal %u changes only the slots of the light that left", step);
    }

    CHECK(print.problems == 0, "the composition validation reports no mismatch");
}

void TestBulkChange(const SyntheticMap &map)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    std::vector<TestLight> lights;

    for (uint32_t i = 0; i < 16; i++)
    {
        lights.push_back({ 3000 + i, 2 + (i % 6) });
    }

    FeedFrame(lists, map.world, lights, frameIndex, &printer, 0.0f, 0, 1);

    const Slots composed = Snapshot(lists, kSmallClusters);

    std::vector<TestLight> changed = lights;

    for (uint32_t i = 0; i < 12; i++)
    {
        changed.erase(changed.begin());
        changed.push_back({ 4000 + i, 2 + (i % 6) });
    }

    FeedFrame(lists, map.world, changed, frameIndex, &printer, 0.0f, 0, 1);

    CHECK(lists.GetStats().composedFrames == 0, "a frame that changes three quarters of the set does not compose");
    CHECK(lists.GetStats().reusedFrames == 1, "a bulk change is patched into the lists");

    const Slots after = Snapshot(lists, kSmallClusters);

    for (uint32_t c = 0; c < kSmallClusters; c++)
    {
        CHECK(after[c].size() == composed[c].size(), "cluster %u keeps its slot count", c);

        for (size_t s = 0; s < after[c].size() && s < composed[c].size(); s++)
        {
            const uint64_t before = composed[c][s];
            const bool survived = std::find_if(changed.begin(), changed.end(), [before](const TestLight &light)
            {
                return light.uid == before;
            }) != changed.end();

            if (survived)
            {
                CHECK(after[c][s] == before, "cluster %u slot %u keeps the light that stayed", c, uint32_t(s));
            }
        }
    }

    CHECK(print.problems == 0, "the composition validation reports no mismatch");
}

void TestMapReload(const SyntheticMap &map, const SyntheticMap &reloaded)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    const std::vector<TestLight> lights = { { 5001, 3 }, { 5002, 4 } };

    FeedFrame(lists, map.world, lights, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 1, "the first map composes");

    FeedFrame(lists, map.world, lights, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 0, "the same map keeps the lists");

    FeedFrame(lists, reloaded.world, lights, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStats().composedFrames == 1, "a new map composes");
}

void TestTailBudget(const SyntheticMap &big)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    std::vector<TestLight> lights;

    for (uint32_t i = 0; i < 200; i++)
    {
        lights.push_back({ 6000 + i, 2 + (i % 100) });
    }

    FeedFrame(lists, big.world, lights, frameIndex, &printer, 1000000.0f, 1, 0);

    const QrClusterLightStats &stats = lists.GetStats();
    const uint32_t clusters = big.world.GetClusterCount();
    const uint32_t cap = uint32_t(Q2_LIGHT_LIST_TAIL_CAPACITY) / clusters;

    CHECK(stats.composedFrames == 1, "the first frame composes the lists");
    CHECK(stats.tailBudgetExceeded == 0, "the tail budget is not exceeded");
    CHECK(stats.candidateMax == 200, "every light is a candidate of every cluster");
    CHECK(stats.clustersWithTail == clusters - 1, "every cluster but the first carries a tail");
    CHECK(stats.tailEntries == (clusters - 1) * cap, "the tails stop at the per-cluster cap");

    uint64_t tailUids[256];
    float    prob[256];
    float    marginal[256];
    uint32_t alias[256];
    float    beta = 0.0f;
    uint32_t count = 0;

    lists.GetClusterTail(1, tailUids, prob, marginal, alias, &beta, 256, &count);

    CHECK(count == cap, "a cluster whose candidates pass the cap publishes exactly the cap");
    CHECK(beta > 0.0f, "the cluster keeps a tail share");
}

void TestStatsResets(const SyntheticMap &map)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    const std::vector<TestLight> first = { { 7001, 3 } };

    FeedFrame(lists, map.world, first, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(lists.GetStatsHistoryCleared(), "a frame that composes drops the whole statistics history");

    FeedFrame(lists, map.world, first, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(!lists.GetStatsHistoryCleared(), "a reused frame drops no history");
    CHECK(lists.GetStatsResetCount() == 0, "a reused frame resets no slot");

    FeedFrame(lists, map.world, { { 7001, 3 }, { 7002, 4 } }, frameIndex, &printer, 0.0f, 0, 1);
    CHECK(!lists.GetStatsHistoryCleared(), "a frame that patches the lists keeps the history of the rest");
    CHECK(lists.GetStatsResetCount() > 0, "the slots the new light takes drop their statistics");
}

void TestRegistryConsistency(const SyntheticMap &map)
{
    qray::ClusterLightLists lists;
    PrintState print;
    qray::UserPrint printer(&CapturePrint, &print);
    uint32_t frameIndex = 0;

    const std::vector<TestLight> frame = { { 8001, 3, { 0.0f, 0.0f, 0.0f } }, { 8002, 4 } };

    CHECK(RT_LIGHT_RENDERER_KEEP_SUM == qray::LightManager::kLightMinColorSum,
          "the admission threshold mirrors the renderer's");
    CHECK(RT_LIGHT_KEEP_SUM > qray::LightManager::kLightMinColorSum,
          "the keep-clamp stays above the renderer's threshold");

    FeedFrame(lists, map.world, frame, frameIndex, &printer, 0.0f, 0, 0);
    CHECK(RegistriesMatch(AdmittedUids(frame, true), NamedUids(lists, kSmallClusters)),
          "the cluster lists name exactly the lights the renderer holds");

    CHECK(!RegistriesMatch(AdmittedUids(frame, false), NamedUids(lists, kSmallClusters)),
          "a light lost from the renderer registry is detected");
}

void FeedParallel(qray::ClusterLightLists &lists, const qray::WorldLights &world,
                  const std::vector<TestLight> &lights, uint32_t &frameIndex, qray::UserPrint *pPrint,
                  uint32_t slices)
{
    std::vector<QrClusterLightSource> sources(lights.size());

    for (size_t i = 0; i < lights.size(); i++)
    {
        sources[i].uniqueID = lights[i].uid;
        sources[i].origin.data[0] = 1000.0f;
        sources[i].origin.data[1] = 1000.0f;
        sources[i].origin.data[2] = 1000.0f;
        sources[i].cluster = lights[i].cluster;
        sources[i].reach = 0.0f;
        sources[i].radius = 0.0f;
        sources[i].clusterCount = 0;
        sources[i].pClusters = nullptr;
        sources[i].power = 1.0f;
    }

    QrClusterLightSourcesUploadInfo upload = {};
    upload.numLights = uint32_t(sources.size());
    upload.pLights = sources.data();
    upload.topUpReach = 1000000.0f;
    upload.allowIncremental = 1;
    upload.allowOverflow = 0;
    upload.validate = 0;

    lists.BeginSources(world, upload, nullptr, pPrint, frameIndex++, slices);

    std::vector<std::thread> threads;

    for (uint32_t s = 0; s < slices; s++)
    {
        threads.emplace_back([&lists, s, slices]()
        {
            lists.RunTopUpSlice(s, slices);
        });
    }

    for (std::thread &thread : threads)
    {
        thread.join();
    }

    lists.FinishSources();
}

void TestParallelSlices(const SyntheticMap &big)
{
    std::vector<TestLight> first;
    std::vector<TestLight> changed;

    for (uint32_t i = 0; i < 200; i++)
    {
        first.push_back({ 9000 + i, 2 + (i % 100) });
        changed.push_back({ 9500 + i, 2 + ((i * 37) % 100) });
    }

    qray::ClusterLightLists serial;
    PrintState serialPrint;
    qray::UserPrint serialPrinter(&CapturePrint, &serialPrint);
    uint32_t serialFrame = 0;

    FeedFrame(serial, big.world, first, serialFrame, &serialPrinter, 1000000.0f, 0, 0);
    FeedFrame(serial, big.world, changed, serialFrame, &serialPrinter, 1000000.0f, 0, 0);

    qray::ClusterLightLists parallel;
    PrintState parallelPrint;
    qray::UserPrint parallelPrinter(&CapturePrint, &parallelPrint);
    uint32_t parallelFrame = 0;

    FeedParallel(parallel, big.world, first, parallelFrame, &parallelPrinter, 8);
    FeedParallel(parallel, big.world, changed, parallelFrame, &parallelPrinter, 8);

    CHECK(parallel.GetStats().composedFrames == 0, "a frame of parallel slices patches the lists");
    CHECK(parallel.GetStatsResetCount() > 0, "a frame of parallel slices collects the slots it changed");
    CHECK(parallel.GetStatsResetCount() == serial.GetStatsResetCount(),
          "parallel slices collect the resets of a whole-frame run (%u against %u)",
          parallel.GetStatsResetCount(), serial.GetStatsResetCount());

    CHECK(serialPrint.problems == 0 && parallelPrint.problems == 0, "the composition validation reports no mismatch");
}

} // namespace

int main()
{
    const SyntheticMap map(kSmallClusters, 1, kSmallRowByte, 1);
    const SyntheticMap reloaded(kSmallClusters * 2, 2, kSmallRowByte, 2);
    const SyntheticMap big(8192, 1024, 0x00, 3, true);

    TestReuseAndEdit(map);
    TestRemovalRun(map);
    TestBulkChange(map);
    TestMapReload(map, reloaded);
    TestTailBudget(big);
    TestStatsResets(map);
    TestRegistryConsistency(map);
    TestParallelSlices(big);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures > 0 ? 1 : 0;
}
