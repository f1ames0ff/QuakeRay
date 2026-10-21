#include "LightGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

qray::LightGridSource MakeSource(uint64_t uid, float x, float y, float z, float reach = 100.0f)
{
    qray::LightGridSource source{};
    source.uid = uid;
    source.origin[0] = x;
    source.origin[1] = y;
    source.origin[2] = z;
    source.reach = reach;
    source.radius = 0.0f;
    source.power = 1.0f;
    return source;
}

uint32_t CenterCell(uint32_t cascade)
{
    const uint32_t local = (8u * 16u + 8u) * 16u + 8u;
    return cascade * qray::LightGrid::kCellsPerCascade + local;
}

std::vector<uint64_t> ReadList(const qray::LightGrid &grid, uint32_t cell)
{
    std::vector<uint64_t> uids(qray::LightGrid::kMaxFastPerCell);
    uint32_t              count = 0;

    grid.GetCellList(cell, uids.data(), uint32_t(uids.size()), &count);
    uids.resize(count);
    return uids;
}

void TestEmptyBuild()
{
    qray::LightGrid grid;
    const float    camera[3] = { 0.0f, 0.0f, 0.0f };

    grid.Build(nullptr, 0, camera);

    const QrLightGridStats &stats = grid.GetStats();

    CHECK(!grid.IsValid(), "an empty source set must not produce a grid");
    CHECK(stats.enabled == 0, "an empty grid must report disabled");
    CHECK(stats.cells == qray::LightGrid::kCells, "the empty grid must still describe its cell count");
}

void TestCenteredLight()
{
    qray::LightGrid grid;
    const float    camera[3] = { 0.0f, 0.0f, 0.0f };
    const qray::LightGridSource source = MakeSource(7, 0.0f, 0.0f, 0.0f, 100.0f);

    grid.Build(&source, 1, camera);

    CHECK(grid.IsValid(), "one source must build a valid grid");

    const QrLightGridStats &stats = grid.GetStats();

    CHECK(stats.enabled == 1, "the built grid must report enabled");
    CHECK(stats.sources == 1, "the grid must report its source count");
    CHECK(stats.occupiedCells > 0, "a light with reach must occupy cells");

    const uint32_t cell = CenterCell(0);
    const std::vector<uint64_t> list = ReadList(grid, cell);

    CHECK(std::find(list.begin(), list.end(), uint64_t(7)) != list.end(),
          "the light must be in the cascade-0 cell that contains its origin");

    const std::vector<uint64_t> coarse = ReadList(grid, CenterCell(1));

    CHECK(std::find(coarse.begin(), coarse.end(), uint64_t(7)) != coarse.end(),
          "the light must be in the cascade-1 cell that contains its origin");
}

void TestFastTailCapacity()
{
    qray::LightGrid grid;
    const float    camera[3] = { 0.0f, 0.0f, 0.0f };
    std::vector<qray::LightGridSource> sources;

    for (uint32_t i = 0; i < 300; i++)
    {
        sources.push_back(MakeSource(uint64_t(i) + 1, 0.0f, 0.0f, 0.0f, 100.0f));
    }

    grid.Build(sources.data(), uint32_t(sources.size()), camera);

    CHECK(grid.IsValid(), "the capacity test must build a valid grid");

    const uint32_t cell = CenterCell(0);
    const std::vector<uint64_t> list = ReadList(grid, cell);

    CHECK(list.size() == qray::LightGrid::kMaxFastPerCell, "the cell list must stop at 64 entries");
    CHECK(grid.GetStats().fastEntries > 0, "the grid must report its stored id count");
    CHECK(grid.GetStats().tailDropped >= 236, "300 candidates must leave 236 candidates over the 64 capacity");
}

void TestNoOverlapWhenOutOfCascade()
{
    qray::LightGrid grid;
    const float    camera[3] = { 0.0f, 0.0f, 0.0f };
    const qray::LightGridSource source = MakeSource(99, 100000.0f, 100000.0f, 100000.0f, 10.0f);

    grid.Build(&source, 1, camera);

    CHECK(grid.IsValid(), "a source outside the cascades must still build a grid");
    CHECK(grid.GetStats().occupiedCells == 0, "a source outside the cascades must occupy no cell");
}

void TestDeterminism()
{
    qray::LightGrid grid;
    const float    camera[3] = { 0.0f, 0.0f, 0.0f };
    std::vector<qray::LightGridSource> sources;

    for (uint32_t i = 0; i < 64; i++)
    {
        sources.push_back(MakeSource(uint64_t(i) + 1, float(i) * 8.0f - 256.0f,
                                     float(i % 8) * 16.0f - 64.0f, 0.0f, 200.0f));
    }

    grid.Build(sources.data(), uint32_t(sources.size()), camera);

    const uint32_t cell = CenterCell(0);
    const std::vector<uint64_t> first = ReadList(grid, cell);

    grid.Build(sources.data(), uint32_t(sources.size()), camera);

    const std::vector<uint64_t> second = ReadList(grid, cell);

    CHECK(first == second, "two builds from the same inputs must produce the same list");
}

}

int main()
{
    TestEmptyBuild();
    TestCenteredLight();
    TestFastTailCapacity();
    TestNoOverlapWhenOutOfCascade();
    TestDeterminism();

    std::printf("%i checks, %i failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
