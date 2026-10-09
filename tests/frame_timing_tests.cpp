#include "CpuFrameProfiler.h"
#include "GeometryBounds.h"
#include "rt_frame_policy.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

void Require(bool value, const char *message)
{
    if (!value)
    {
        throw std::runtime_error(message);
    }
}

void TestFramePolicy()
{
    Require(RT_ShouldRenderUiOnly(0, 0), "startup uses UI-only rendering");
    Require(RT_ShouldRenderUiOnly(0, 1), "an absent world uses UI-only rendering");
    Require(RT_ShouldRenderUiOnly(1, 0), "loading preserves UI-only rendering");
    Require(!RT_ShouldRenderUiOnly(1, 1), "a loaded game and its pause menu keep the scene");

    Require(RT_ShouldSkipConsoleDraw(1, 1, 1.0f), "an opaque disconnected menu hides console draws");
    Require(!RT_ShouldSkipConsoleDraw(0, 1, 1.0f), "the console remains visible without a menu");
    Require(!RT_ShouldSkipConsoleDraw(1, 0, 1.0f), "a non-forced console remains visible");
    Require(!RT_ShouldSkipConsoleDraw(1, 1, 0.5f), "a translucent preview preserves console draws");
    Require(!RT_ShouldSkipConsoleDraw(1, 1, 0.0f), "a hidden menu preserves console draws");

    Require(RT_EndTaskResultMergesNow(7, 7), "a frame waiting for its end task takes the result");
    Require(!RT_EndTaskResultMergesNow(7, 8), "a stale waiting frame does not take a newer result");
    Require(!RT_EndTaskResultMergesNow(0, 7), "a frame that never waits takes no result");
    Require(!RT_EndTaskResultMergesNow(7, 0), "a zero serial never merges");

    Require(RT_EndFrameConsumesEarlyResult(7, 7), "a frame consumes its own early result");
    Require(!RT_EndFrameConsumesEarlyResult(7, 8), "a frame does not consume a foreign early result");
    Require(!RT_EndFrameConsumesEarlyResult(0, 7), "serial mode consumes no task results");
    Require(!RT_EndFrameConsumesEarlyResult(7, 0), "a frame without an early result consumes nothing");
}

void TestEndTaskCompletionOrder()
{
    {
        unsigned pending = 0;
        const unsigned serial = 5;
        double sample = 0.0;

        Require(!RT_EndTaskResultMergesNow(pending, serial), "a result without a waiting frame stores early");
        pending = serial;
        Require(!RT_EndTaskResultMergesNow(pending, serial + 1), "a foreign result does not merge into the wait");
        Require(RT_EndTaskResultMergesNow(pending, serial), "the frame's own result merges into the wait");
        sample += 7.5;
        pending = 0;
        Require(!RT_EndTaskResultMergesNow(pending, serial), "a merged result cannot merge twice");
        Require(sample == 7.5, "the late result lands exactly once");
    }

    {
        unsigned finished = 0;
        const unsigned serial = 6;
        double sample = 0.0;

        Require(!RT_EndFrameConsumesEarlyResult(serial, finished), "a frame consumes nothing before its result");
        finished = serial;
        Require(!RT_EndFrameConsumesEarlyResult(serial + 1, finished), "a frame does not consume a foreign early result");
        Require(RT_EndFrameConsumesEarlyResult(serial, finished), "a frame consumes its own early result");
        sample += 6.25;
        finished = 0;
        Require(!RT_EndFrameConsumesEarlyResult(serial, finished), "a consumed early result cannot be taken twice");
        Require(sample == 6.25, "the early result lands exactly once");
    }

    Require(!RT_EndFrameConsumesEarlyResult(0, 9), "the serial frame never consumes a task result");
}

void TestCacheGeneration()
{
    Require(RT_CacheGenerationStale(0, 1), "a table that was never used is stale");
    Require(!RT_CacheGenerationStale(1, 1), "a refreshed table is current");
    Require(RT_CacheGenerationStale(1, 2), "a reset makes every thread's table stale");
    Require(!RT_CacheGenerationStale(2, 2), "a table refreshed after the reset is current");
}

void TestReportRunOwnership()
{
    unsigned active = 1;

    Require(RT_ReportTakeRun(&active), "the first caller owns the finished run");
    Require(active == 0, "taking the run clears it");
    Require(!RT_ReportTakeRun(&active), "a re-entrant caller finds the run taken");

    active = 0;
    Require(!RT_ReportTakeRun(&active), "an inactive run is never reported");
}

void TestCpuProfiler()
{
    qray::CpuFrameProfiler profiler;
    Require(!profiler.IsEnabled(), "profiling is disabled by default");
    {
        qray::CpuProfileScope scope(&profiler, QR_CPU_PASS_UI);
    }
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_UI] == 0.0f, "disabled scopes record no time");

    profiler.Reset(true);
    profiler.Begin(QR_CPU_PASS_PREPARE);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    profiler.End(QR_CPU_PASS_PREPARE);
    const float first = profiler.GetMilliseconds()[QR_CPU_PASS_PREPARE];
    Require(first > 0.0f && std::isfinite(first), "enabled profiling records a finite interval");
    profiler.End(QR_CPU_PASS_PREPARE);
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_PREPARE] == first, "an interval ends only once");

    {
        qray::CpuProfileScope scope(&profiler, QR_CPU_PASS_PREPARE);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        scope.Finish();
        const float finished = profiler.GetMilliseconds()[QR_CPU_PASS_PREPARE];
        scope.Finish();
        Require(profiler.GetMilliseconds()[QR_CPU_PASS_PREPARE] == finished, "scope finish is idempotent");
    }
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_PREPARE] > first, "multiple intervals accumulate");

    profiler.Begin(QR_CPU_PASS_COUNT);
    profiler.End(QR_CPU_PASS_COUNT);
    profiler.Begin(static_cast<QrCpuPassIndex>(-1));
    profiler.End(static_cast<QrCpuPassIndex>(-1));

    profiler.Reset(false);
    Require(!profiler.IsEnabled(), "reset can disable profiling");
    for (float value : profiler.GetMilliseconds())
    {
        Require(value == 0.0f, "reset clears every pass");
    }

    profiler.Reset(true);
    profiler.Begin(QR_CPU_PASS_UI);
    profiler.Reset(true);
    profiler.End(QR_CPU_PASS_UI);
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_UI] == 0.0f, "reset discards an unfinished interval");
}

void CheckBounds(const QrGeometryUploadInfo &info, bool preinitialized)
{
    bool scalarInitialized = preinitialized;
    bool simdInitialized = preinitialized;
    float scalarMin[3] = {-7.0f, -3.0f, -11.0f};
    float scalarMax[3] = {13.0f, 5.0f, 17.0f};
    float simdMin[3] = {-7.0f, -3.0f, -11.0f};
    float simdMax[3] = {13.0f, 5.0f, 17.0f};
    qray::AccumulateGeometryBoundsScalar(info, scalarInitialized, scalarMin, scalarMax);
    qray::AccumulateGeometryBounds(info, simdInitialized, simdMin, simdMax);
    Require(scalarInitialized == simdInitialized, "geometry bounds initialization matches the scalar path");
    for (int axis = 0; axis < 3; ++axis)
    {
        Require(std::bit_cast<uint32_t>(scalarMin[axis]) == std::bit_cast<uint32_t>(simdMin[axis]),
                "geometry minimum is bit-identical to the scalar path");
        Require(std::bit_cast<uint32_t>(scalarMax[axis]) == std::bit_cast<uint32_t>(simdMax[axis]),
                "geometry maximum is bit-identical to the scalar path");
    }
}

void TestGeometryBounds()
{
    QrGeometryUploadInfo info{};
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1.0f;
    CheckBounds(info, false);
    CheckBounds(info, true);

    std::vector<QrVertex> vertices(8192);
    std::mt19937 random(18435);
    std::uniform_real_distribution<float> coordinate(-20000.0f, 20000.0f);
    std::uniform_real_distribution<float> coefficient(-3.0f, 3.0f);
    for (auto &vertex : vertices)
        for (float &value : vertex.position)
            value = coordinate(random);
    vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    vertices[1].position[1] = std::numeric_limits<float>::infinity();
    vertices[2].position[2] = -std::numeric_limits<float>::infinity();
    vertices[3].position[0] = 1.0e7f;
    vertices[4].position[0] = std::nextafter(1.0e7f, std::numeric_limits<float>::infinity());
    vertices[5].position[1] = -1.0e7f;
    vertices[6].position[1] = std::nextafter(-1.0e7f, -std::numeric_limits<float>::infinity());
    info.vertexCount = uint32_t(vertices.size());
    info.pVertices = vertices.data();
    CheckBounds(info, false);
    CheckBounds(info, true);

    for (int transform = 0; transform < 24; ++transform)
    {
        for (auto &row : info.transform.matrix)
            for (float &value : row)
                value = coefficient(random);
        info.transform.matrix[0][3] = coordinate(random);
        info.transform.matrix[1][3] = coordinate(random);
        info.transform.matrix[2][3] = coordinate(random);
        CheckBounds(info, false);
        CheckBounds(info, true);
    }

    std::array<QrVertex, 3> invalid{};
    invalid[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    invalid[1].position[1] = std::numeric_limits<float>::infinity();
    invalid[2].position[2] = 2.0e8f;
    info = {};
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1.0f;
    info.vertexCount = uint32_t(invalid.size());
    info.pVertices = invalid.data();
    CheckBounds(info, false);
    CheckBounds(info, true);
    info.transform.matrix[0][3] = std::numeric_limits<float>::infinity();
    CheckBounds(info, false);

    std::array<QrVertex, 2> zeros{};
    zeros[0].position[0] = zeros[0].position[1] = zeros[0].position[2] = -0.0f;
    info = {};
    info.vertexCount = uint32_t(zeros.size());
    info.pVertices = zeros.data();
    for (auto &row : info.transform.matrix)
        for (float &value : row)
            value = -0.0f;
    CheckBounds(info, false);
    CheckBounds(info, true);
}

void BenchmarkGeometryBounds()
{
    std::vector<QrVertex> vertices(65536);
    std::mt19937 random(61824);
    std::uniform_real_distribution<float> coordinate(-8192.0f, 8192.0f);
    for (auto &vertex : vertices)
        for (float &value : vertex.position)
            value = coordinate(random);
    QrGeometryUploadInfo info{};
    info.vertexCount = uint32_t(vertices.size());
    info.pVertices = vertices.data();
    info.transform = {{{0.8f, -0.6f, 0.2f, 32.0f}, {0.6f, 0.8f, -0.1f, -17.0f}, {0.0f, 0.15f, 1.0f, 4.0f}}};
    using Accumulate = void (*)(const QrGeometryUploadInfo &, bool &, float *, float *);
    auto measure = [&](Accumulate accumulate)
    {
        volatile float checksum = 0.0f;
        const auto begin = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 32; ++repeat)
        {
            bool initialized = false;
            float minimum[3] = {};
            float maximum[3] = {};
            accumulate(info, initialized, minimum, maximum);
            checksum = minimum[0] + maximum[2];
        }
        Require(std::isfinite(checksum), "bounds benchmark has a finite result");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    };
    measure(qray::AccumulateGeometryBoundsScalar);
    measure(qray::AccumulateGeometryBounds);
    double scalar = 0.0;
    double simd = 0.0;
    for (int round = 0; round < 4; ++round)
    {
        if (round % 2)
        {
            simd += measure(qray::AccumulateGeometryBounds);
            scalar += measure(qray::AccumulateGeometryBoundsScalar);
        }
        else
        {
            scalar += measure(qray::AccumulateGeometryBoundsScalar);
            simd += measure(qray::AccumulateGeometryBounds);
        }
    }
    std::cout << "Geometry bounds: scalar " << scalar / 4.0 << " ms, SIMD " << simd / 4.0
              << " ms, ratio " << scalar / simd << "x; 2097152 vertices per arm\n";
}

}

int main(int argc, char **argv)
{
    try
    {
        TestFramePolicy();
        TestEndTaskCompletionOrder();
        TestCacheGeneration();
        TestReportRunOwnership();
        TestCpuProfiler();
        TestGeometryBounds();
        std::cout << "Frame policy, end-task routing, cache and report tests passed\n";
        if (argc == 2 && std::strcmp(argv[1], "--bench-bounds") == 0)
            BenchmarkGeometryBounds();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#include <array>
#include <bit>
