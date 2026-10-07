#include "CpuFrameProfiler.h"
#include "rt_frame_policy.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

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

}

int main()
{
    try
    {
        TestFramePolicy();
        TestCpuProfiler();
        std::cout << "Frame policy and CPU timing tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
