#include "rt_prof_window.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{

void Require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct Fixture
{
    rt_prof_window_t window{};
    std::mutex mutex;
    std::atomic<unsigned> lockAttempts{};
    double sum{};
    unsigned rendererSamples{};
    unsigned resets{};
    unsigned reportedFrames{};
    unsigned reportedRendererSamples{};
    unsigned reportedId{};
    double reportedAverage{};
    double reportedElapsed{};
    double recordValue{8};

    rt_prof_window_sync_t Sync()
    {
        return {this, Lock, Unlock};
    }

    static void Lock(void *context)
    {
        auto &f = *static_cast<Fixture *>(context);
        ++f.lockAttempts;
        f.mutex.lock();
    }

    static void Unlock(void *context)
    {
        static_cast<Fixture *>(context)->mutex.unlock();
    }

    static void Clear(void *context)
    {
        auto &f = *static_cast<Fixture *>(context);
        f.sum = 0;
        f.rendererSamples = 0;
    }

    static void Reset(void *context)
    {
        ++static_cast<Fixture *>(context)->resets;
        Clear(context);
    }

    static void Record(void *context)
    {
        auto &f = *static_cast<Fixture *>(context);
        f.sum += f.recordValue;
        ++f.rendererSamples;
    }

    static void Publish(void *context, unsigned frames, double elapsed, unsigned id)
    {
        auto &f = *static_cast<Fixture *>(context);
        f.reportedElapsed = elapsed;
        f.reportedFrames = frames;
        f.reportedRendererSamples = f.rendererSamples;
        f.reportedAverage = f.sum / frames;
        f.reportedId = id;
    }
};

void TestCompletionOrders()
{
    for (bool early : {false, true})
    {
        Fixture f;
        auto sync = f.Sync();
        for (unsigned frame = 0; frame < 8; ++frame)
        {
            const double now = frame;
            f.recordValue = 8 + frame;
            RT_ProfWindowBeginFrame(&f.window, &sync, 1, 1, now, Fixture::Reset, &f);
            RT_ProfWindowSubmitEnd(&f.window, &sync);
            if (early)
            {
                RT_ProfWindowRecordEnd(&f.window, &sync, Fixture::Record, &f);
                Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 0.1, 0.05,
                    Fixture::Publish, Fixture::Clear, &f), "an open frame cannot publish an early end result");
            }
            RT_ProfWindowEndFrame(&f.window, &sync);
            if (!early)
            {
                Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 0.1, 0.05,
                    Fixture::Publish, Fixture::Clear, &f), "a pending end result defers publication");
                RT_ProfWindowRecordEnd(&f.window, &sync, Fixture::Record, &f);
            }
            Require(RT_ProfWindowTryPublish(&f.window, &sync, now + 0.2, 0.05,
                Fixture::Publish, Fixture::Clear, &f), "each completed frame publishes");
            Require(f.reportedFrames == 1 && f.reportedRendererSamples == 1, "the same frame owns its result and denominator");
            Require(f.reportedAverage == f.recordValue && f.reportedId == frame + 1, "no result moves into another window");
            Require(f.reportedElapsed > 0, "a report has a positive interval");
            Require(f.sum == 0 && f.window.pending == 0 && f.window.frames == 0, "publication clears only a complete window");
            Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 0.4, 0.05,
                Fixture::Publish, Fixture::Clear, &f), "an empty window never republishes");
        }
        Require(f.resets == 1, "steady-state publication does not reinitialize the profiler");
    }
}

void TestEnableAndReenable()
{
    Fixture f;
    auto sync = f.Sync();
    RT_ProfWindowBeginFrame(&f.window, &sync, 0, 0, 0, Fixture::Reset, &f);
    f.sum = 100;
    Require(!RT_ProfWindowTryPublish(&f.window, &sync, 1, 0.05, Fixture::Publish, Fixture::Clear, &f), "a disabled profiler cannot publish");

    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        const double now = 10 * (cycle + 1);
        RT_ProfWindowBeginFrame(&f.window, &sync, 1, 1, now, Fixture::Reset, &f);
        const unsigned resets = f.resets;
        RT_ProfWindowSubmitEnd(&f.window, &sync);
        RT_ProfWindowEndFrame(&f.window, &sync);
        Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 0.1, 0.05,
            Fixture::Publish, Fixture::Clear, &f), "the first delayed frame waits without resetting");
        Require(f.resets == resets && f.window.frames == 1 && f.window.pending == 1, "activation preserves the in-flight first frame");
        RT_ProfWindowRecordEnd(&f.window, &sync, Fixture::Record, &f);
        Require(RT_ProfWindowTryPublish(&f.window, &sync, now + 0.2, 0.05,
            Fixture::Publish, Fixture::Clear, &f), "the first completed frame publishes after enable");
        Require(f.reportedAverage == 8 && f.reportedFrames == 1 && f.reportedRendererSamples == 1, "the first window has an exact denominator");
        RT_ProfWindowBeginFrame(&f.window, &sync, 0, 0, now + 0.3, Fixture::Reset, &f);
        Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 1, 0.05,
            Fixture::Publish, Fixture::Clear, &f), "disable prevents further publications");
    }
}

void TestMultiFrameWindow()
{
    Fixture f;
    auto sync = f.Sync();
    for (unsigned frame = 0; frame < 4; ++frame)
    {
        const double now = frame * 0.2;
        f.recordValue = 8 + frame;
        RT_ProfWindowBeginFrame(&f.window, &sync, 1, 1, now, Fixture::Reset, &f);
        RT_ProfWindowSubmitEnd(&f.window, &sync);
        RT_ProfWindowEndFrame(&f.window, &sync);
        RT_ProfWindowRecordEnd(&f.window, &sync, Fixture::Record, &f);
        Require(!RT_ProfWindowTryPublish(&f.window, &sync, now + 0.1, 1,
            Fixture::Publish, Fixture::Clear, &f), "an incomplete reporting interval retains all frame data");
    }
    Require(RT_ProfWindowTryPublish(&f.window, &sync, 1.2, 1, Fixture::Publish, Fixture::Clear, &f), "several late frames publish together");
    Require(f.reportedFrames == 4 && f.reportedRendererSamples == 4 && f.reportedAverage == 9.5, "a multi-frame window has the exact sum and denominator");
}

void TestRecordAndPublishAreAtomic()
{
    Fixture f;
    auto sync = f.Sync();
    RT_ProfWindowBeginFrame(&f.window, &sync, 1, 1, 0, Fixture::Reset, &f);
    RT_ProfWindowSubmitEnd(&f.window, &sync);
    RT_ProfWindowEndFrame(&f.window, &sync);

    struct PausedRecord
    {
        Fixture *fixture;
        std::promise<bool> entered;
        std::shared_future<void> release;
    };
    std::promise<void> release;
    PausedRecord record{&f, {}, release.get_future().share()};
    auto entered = record.entered.get_future();
    auto worker = std::async(std::launch::async, [&] {
        RT_ProfWindowRecordEnd(&f.window, &sync, [](void *context) {
            auto &r = *static_cast<PausedRecord *>(context);
            r.entered.set_value(r.fixture->window.pending == 1);
            r.release.wait();
            Fixture::Record(r.fixture);
        }, &record);
    });
    const bool pendingDuringWrite = entered.get();
    std::promise<void> attempted;
    auto attempt = attempted.get_future();
    auto publisher = std::async(std::launch::async, [&] {
        attempted.set_value();
        return RT_ProfWindowTryPublish(&f.window, &sync, 1, 0.05, Fixture::Publish, Fixture::Clear, &f);
    });
    attempt.wait();
    const bool premature = publisher.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
    release.set_value();
    worker.get();
    const bool published = publisher.get();
    Require(pendingDuringWrite, "pending remains outstanding throughout the result write");
    Require(!premature && published, "publication waits for the complete result write");
    Require(f.reportedAverage == 8 && f.reportedFrames == 1 && f.reportedRendererSamples == 1, "the atomic snapshot includes both result and frame");
    Require(f.window.pending == 0 && f.sum == 0, "no late data remains after publication");
}

}

int main()
{
    try
    {
        TestCompletionOrders();
        TestEnableAndReenable();
        TestMultiFrameWindow();
        TestRecordAndPublishAreAtomic();
        std::cout << "Profiler window lifecycle and concurrent publication tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
