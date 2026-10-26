#include "CpuFrameProfiler.h"
#include "CommandBufferManager.h"
#include "FrameAcquireGate.h"
#include "GeometryBounds.h"
#include "RHI/DynamicBlasShape.h"
#include "rt_frame_policy.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace qray
{

class TestCommandBufferManager
{
public:
    explicit TestCommandBufferManager(VkCommandBuffer command)
        : command(command)
    {
    }

    VkCommandBuffer StartGraphicsCmd()
    {
        ++startCount;
        return command;
    }

    uint32_t GetStartCount() const
    {
        return startCount;
    }

private:
    VkCommandBuffer command;
    uint32_t startCount = 0;
};

}

// Keep the production header's slot logic while substituting its command-buffer factory in this TU.
#define CommandBufferManager TestCommandBufferManager
#include "FrameState.h"
#undef CommandBufferManager

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

    profiler.Add(QR_CPU_PASS_FRAME_FENCE_WAIT, 1.25f);
    profiler.Add(QR_CPU_PASS_FRAME_FENCE_WAIT, 0.75f);
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_FRAME_FENCE_WAIT] == 2.0f,
            "measured intervals accumulate without a scope");
    profiler.Add(QR_CPU_PASS_COUNT, 1.0f);
    profiler.Add(static_cast<QrCpuPassIndex>(-1), 1.0f);

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

    profiler.Add(QR_CPU_PASS_PRESENT_WAIT, 1.0f);
    Require(profiler.GetMilliseconds()[QR_CPU_PASS_PRESENT_WAIT] == 1.0f,
            "measured intervals can be added after reset");
}

void TestFrameStateSlots()
{
    qray::FrameState state;
    const auto commandA = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x10));
    const auto commandB = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x20));
    const auto semaphoreA = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x30));
    const auto semaphoreB = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x40));

    Require(state.GetFrameIndex() == qray::MAX_FRAMES_IN_FLIGHT - 1,
            "frame state starts on the last slot");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == VK_NULL_HANDLE,
            "the initial slot has no wait semaphore");

    state.SetSemaphore(semaphoreA, VK_PIPELINE_STAGE_TRANSFER_BIT);
    state.OnBeginFrame(commandA);
    Require(state.WasFrameStarted(), "the first slot starts independently");

    Require(state.IncrementFrameIndexAndGet() == 0, "the next frame advances to slot zero");
    Require(!state.WasFrameStarted(), "started state does not leak into the next slot");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == VK_NULL_HANDLE,
            "wait semaphore does not leak into the next slot");

    state.SetSemaphore(semaphoreB, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    state.OnBeginFrame(commandB);
    Require(state.GetCmdBuffer() == commandB, "the second slot owns its command buffer");

    Require(state.IncrementFrameIndexAndGet() == qray::MAX_FRAMES_IN_FLIGHT - 1,
            "the third frame returns to the first slot");
    Require(state.WasFrameStarted(), "the first slot keeps its in-flight state");
    Require(state.GetCmdBuffer() == commandA, "the first command buffer is preserved");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == semaphoreA,
            "the first wait semaphore is preserved");
    Require(waitStage == VK_PIPELINE_STAGE_TRANSFER_BIT,
            "the first semaphore wait stage is preserved");
    state.OnEndFrame();
    Require(!state.WasFrameStarted(), "ending the first slot leaves only that slot idle");

    Require(state.IncrementFrameIndexAndGet() == 0, "the fourth frame returns to slot zero");
    Require(state.GetCmdBuffer() == commandB, "the second command buffer is preserved");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == semaphoreB,
            "the second wait semaphore is preserved");
    Require(waitStage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            "the second semaphore wait stage is preserved");
    state.OnEndFrame();
}

void TestFrameStatePreFrameSlotOwnership()
{
    qray::FrameState state;
    const auto preFrameCommand = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x50));
    const auto frameCommandN = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x60));
    const auto frameCommandNPlusOne = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x70));
    const auto semaphoreN = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x80));
    const auto semaphoreNPlusOne = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x90));
    const uint32_t slotN = state.GetFrameIndex();
    const uint32_t slotNPlusOne = (slotN + 1) % qray::MAX_FRAMES_IN_FLIGHT;
    auto commandBuffers = std::make_shared<qray::TestCommandBufferManager>(preFrameCommand);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    Require(slotN != slotNPlusOne, "the tested pre-frame slots are distinct");

    state.SetSemaphore(semaphoreN, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Require(state.GetCmdBufferForMaterials(commandBuffers) == preFrameCommand,
            "the pre-frame command is created in the current slot");
    Require(commandBuffers->GetStartCount() == 1, "the pre-frame command is created once");

    state.OnBeginFrame(frameCommandN);
    Require(state.GetCmdBufferForMaterials(commandBuffers) == frameCommandN,
            "materials use the frame command after the slot's frame starts");
    Require(commandBuffers->GetStartCount() == 1, "an active frame creates no pre-frame command");

    Require(state.IncrementFrameIndexAndGet() == slotNPlusOne, "the frame advances past the pre-frame slot");
    Require(!state.WasFrameStarted(), "the next slot has no frame command");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == VK_NULL_HANDLE,
            "the next slot has no wait semaphore");
    Require(waitStage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            "the next slot keeps its default wait stage");
    Require(state.GetPreFrameCmdAndRemove(slotNPlusOne) == VK_NULL_HANDLE,
            "the next slot does not own the previous slot's pre-frame command");

    Require(state.GetPreFrameCmdAndRemove(slotN) == preFrameCommand,
            "the pre-frame command is retrieved from the slot where it was created");
    Require(state.GetPreFrameCmdAndRemove(slotN) == VK_NULL_HANDLE,
            "the retrieved pre-frame command cannot be taken twice");

    state.SetSemaphore(semaphoreNPlusOne, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    state.OnBeginFrame(frameCommandNPlusOne);
    Require(state.GetCmdBuffer() == frameCommandNPlusOne,
            "the next slot owns only its frame command");

    Require(state.IncrementFrameIndexAndGet() == slotN, "the frame wraps back to the pre-frame slot");
    Require(state.GetCmdBuffer() == frameCommandN, "the original slot keeps its frame command");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == semaphoreN,
            "the original slot keeps its wait semaphore");
    Require(waitStage == VK_PIPELINE_STAGE_TRANSFER_BIT,
            "the original slot keeps its semaphore wait stage");
    state.OnEndFrame();

    Require(state.IncrementFrameIndexAndGet() == slotNPlusOne, "the frame returns to the next slot");
    Require(state.GetCmdBuffer() == frameCommandNPlusOne, "the next slot keeps its frame command");
    Require(state.GetSemaphoreForWaitAndRemove(&waitStage) == semaphoreNPlusOne,
            "the next slot keeps its wait semaphore");
    Require(waitStage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            "the next slot keeps its semaphore wait stage");
    state.OnEndFrame();
}

void TestFrameAcquireGate()
{
    qray::FrameAcquireGate gate;
    std::atomic<uint32_t> calls{0};
    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
    std::atomic<bool> waiterStarted{false};
    std::atomic<bool> waiterReturned{false};

    std::thread owner([&] {
        gate.Acquire([&] {
            ++calls;
            running.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        });
    });

    while (!running.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    std::thread waiter([&] {
        waiterStarted.store(true, std::memory_order_release);
        gate.Acquire([&] {
            ++calls;
        });
        waiterReturned.store(true, std::memory_order_release);
    });

    while (!waiterStarted.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Require(!waiterReturned.load(std::memory_order_acquire),
            "a concurrent acquire waits for the owner");

    release.store(true, std::memory_order_release);
    owner.join();
    waiter.join();

    Require(calls.load(std::memory_order_acquire) == 1,
            "concurrent acquire callers execute exactly once");
    Require(waiterReturned.load(std::memory_order_acquire),
            "a concurrent acquire returns after completion");

    gate.Acquire([&] {
        ++calls;
    });
    Require(calls.load(std::memory_order_acquire) == 1,
            "a completed acquire is not repeated");

    gate.Reset();
    gate.Acquire([&] {
        ++calls;
    });
    Require(calls.load(std::memory_order_acquire) == 2,
            "reset opens the next frame's acquire gate");

    gate.Reset();
    try
    {
        gate.Acquire([] {
            throw std::runtime_error("injected acquire failure");
        });
        Require(false, "an acquire failure is propagated");
    }
    catch (const std::runtime_error &error)
    {
        Require(std::strcmp(error.what(), "injected acquire failure") == 0,
                "the original acquire failure reaches the caller");
    }

    try
    {
        gate.Acquire([&] {
            ++calls;
        });
        Require(false, "a recorded acquire failure is not forgotten");
    }
    catch (const std::runtime_error &error)
    {
        Require(std::strcmp(error.what(), "injected acquire failure") == 0,
                "later callers observe the recorded acquire failure");
    }
    Require(calls.load(std::memory_order_acquire) == 2,
            "a failed acquire is not retried as a second acquisition");
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

void TestDynamicBlasShape()
{
    using qray::rhi::DynamicShapeFits;
    const std::vector<uint32_t> envelope = { 12, 100, 80, 1, 1, 40, 200, 80, 1, 1 };

    Require(DynamicShapeFits(envelope, envelope), "an identical shape fits the envelope");
    Require(DynamicShapeFits(envelope, { 12, 100, 80, 1, 1 }), "a shorter geometry count fits the envelope");
    Require(DynamicShapeFits(envelope, { 8, 90, 80, 1, 1, 40, 200, 80, 1, 1 }), "shrunk bounds fit the envelope");
    Require(DynamicShapeFits(envelope, std::vector<uint32_t>()), "an empty shape fits the envelope");

    Require(!DynamicShapeFits(envelope, { 12, 100, 80, 1, 1, 40, 200, 80, 1, 1, 5, 5, 80, 1, 1 }),
            "a growing geometry count does not fit");
    Require(!DynamicShapeFits(envelope, { 13, 100, 80, 1, 1, 40, 200, 80, 1, 1 }),
            "a growing primitive count does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 101, 80, 1, 1, 40, 200, 80, 1, 1 }),
            "a growing vertex count does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 100, 64, 1, 1, 40, 200, 80, 1, 1 }),
            "a changed stride does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 100, 80, 0, 1, 40, 200, 80, 1, 1 }),
            "a changed index type does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 100, 80, 1, 0, 40, 200, 80, 1, 1 }),
            "a changed transform presence does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 100, 80, 1 }), "a malformed shape does not fit");
    Require(!DynamicShapeFits({ 12, 100, 80 }, { 12, 100, 80 }), "a malformed envelope does not fit");
    Require(!DynamicShapeFits(envelope, { 12, 100, 80, 1, 1, 41, 200, 80, 1, 1 }),
            "growth in a later geometry does not fit");

    using qray::rhi::DynamicShapeSortedFits;

    // The sorted test is rank-wise dominance of the primitive counts (see DynamicBlasShape.h): a
    // reordered list and any per-geometry counts within the envelope's sorted counts fit, while a
    // count above the envelope's at any rank does not. The stride, the vertex count and the
    // transform presence are not compared - the allocation does not depend on them (the filter key
    // fixes the stride and the format, and the transform only selects a matrix address) - so the
    // identity-change cases below are accepted.
    Require(DynamicShapeSortedFits(envelope, envelope), "an identical shape fits the sorted envelope");
    Require(DynamicShapeSortedFits(envelope, { 40, 200, 80, 1, 1, 12, 100, 80, 1, 1 }),
            "a permuted shape fits the sorted envelope");
    Require(DynamicShapeSortedFits(envelope, { 40, 200, 64, 0, 0, 12, 100, 80, 1, 1 }),
            "an identity change alone (stride, indexedness, transform) fits the sorted envelope");
    Require(DynamicShapeSortedFits(envelope, { 40, 200, 80, 1, 1 }),
            "a shorter permuted shape fits the sorted envelope");
    Require(DynamicShapeSortedFits(envelope, { 12, 99, 80, 1, 1, 39, 199, 80, 1, 1 }),
            "counts below the sorted envelope fit");
    Require(DynamicShapeSortedFits(envelope, std::vector<uint32_t>()), "an empty shape fits the sorted envelope");
    Require(!DynamicShapeSortedFits(envelope, { 12, 100, 80, 1, 1, 41, 200, 80, 1, 1 }),
            "a primitive count above the sorted envelope does not fit");
    Require(!DynamicShapeSortedFits(envelope, { 13, 100, 80, 1, 1, 40, 200, 80, 1, 1 }),
            "a second count above the sorted envelope does not fit");
    Require(!DynamicShapeSortedFits(envelope, { 12, 100, 80, 1, 1, 40, 200, 80, 1, 1, 5, 5, 80, 1, 1 }),
            "a growing geometry count does not fit the sorted envelope");
    Require(!DynamicShapeSortedFits(envelope, { 12, 100, 80, 1 }), "a malformed shape does not fit the sorted envelope");
    Require(!DynamicShapeSortedFits({ 12, 100, 80 }, { 12, 100, 80 }),
            "a malformed envelope does not fit the sorted envelope");
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
        TestFrameStateSlots();
        TestFrameStatePreFrameSlotOwnership();
        TestFrameAcquireGate();
        TestGeometryBounds();
        TestDynamicBlasShape();
        std::cout << "Frame policy, end-task routing, cache, report, CPU timing, frame state/pre-frame ownership, acquire gate, geometry bounds and dynamic BLAS shape tests passed\n";
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
