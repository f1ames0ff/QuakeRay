#pragma once

#include <qray/qray.h>

#include <array>
#include <chrono>

namespace qray
{

class CpuFrameProfiler
{
public:
    void Reset(bool enable)
    {
        enabled = enable;
        milliseconds.fill(0.0f);
        active.fill(false);
    }

    bool IsEnabled() const { return enabled; }

    void Begin(QrCpuPassIndex pass)
    {
        const auto index = static_cast<uint32_t>(pass);
        if (enabled && index < QR_CPU_PASS_COUNT)
        {
            starts[index] = Clock::now();
            active[index] = true;
        }
    }

    void End(QrCpuPassIndex pass)
    {
        const auto index = static_cast<uint32_t>(pass);
        if (enabled && index < QR_CPU_PASS_COUNT && active[index])
        {
            milliseconds[index] += std::chrono::duration<float, std::milli>(Clock::now() - starts[index]).count();
            active[index] = false;
        }
    }

    const std::array<float, QR_CPU_PASS_COUNT> &GetMilliseconds() const { return milliseconds; }

private:
    using Clock = std::chrono::steady_clock;

    bool enabled = false;
    std::array<Clock::time_point, QR_CPU_PASS_COUNT> starts{};
    std::array<bool, QR_CPU_PASS_COUNT> active{};
    std::array<float, QR_CPU_PASS_COUNT> milliseconds{};
};

class CpuProfileScope
{
public:
    CpuProfileScope(CpuFrameProfiler *pProfiler, QrCpuPassIndex index)
        : profiler(pProfiler), pass(index)
    {
        if (profiler != nullptr)
        {
            profiler->Begin(pass);
        }
    }

    ~CpuProfileScope() { Finish(); }

    CpuProfileScope(const CpuProfileScope &) = delete;
    CpuProfileScope &operator=(const CpuProfileScope &) = delete;

    void Finish()
    {
        if (profiler != nullptr)
        {
            profiler->End(pass);
            profiler = nullptr;
        }
    }

private:
    CpuFrameProfiler *profiler;
    QrCpuPassIndex pass;
};

}
