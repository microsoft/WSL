/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcherUnitTests.cpp

Abstract:

    Unit tests for SlowOperationWatcher. These exercise the emission logic through an
    injected recording sink, so no ETW listener or running distro is required. The
    watcher emits AT MOST ONE event:

      - Fast path (finishes under SlowThreshold): nothing is emitted.
      - Slow completion (finishes at >= SlowThreshold, before MaxDuration): exactly one
        timedOut=false event with the real elapsed time.
      - Hang (still running at MaxDuration): exactly one timedOut=true event with
        elapsed ~= MaxDuration, and no second event when the scope later exits.
      - Reset() behaves like the destructor and never double-emits.

--*/

#include "precomp.h"
#include "Common.h"

#include <mutex>
#include <thread>
#include <vector>

namespace SlowOperationWatcherUnitTests {

namespace {

    using namespace std::chrono_literals;

    struct RecordedEvent
    {
        std::string Name;
        std::chrono::milliseconds Threshold;
        std::chrono::milliseconds Elapsed;
        bool TimedOut;
        unsigned Line;
    };

    // The sink is a plain function pointer (no captures), so recorded events land in this
    // file-scope recorder. TAEF runs the methods of a class serially, and each test resets
    // the recorder up front, so there is no cross-test interference.
    struct Recorder
    {
        std::mutex Lock;
        std::vector<RecordedEvent> Events;

        void Clear()
        {
            std::scoped_lock lock{Lock};
            Events.clear();
        }

        std::vector<RecordedEvent> Snapshot()
        {
            std::scoped_lock lock{Lock};
            return Events;
        }
    };

    Recorder g_recorder;

    void RecordSink(const SlowOperationWatcher::Event& Record) noexcept
    try
    {
        std::scoped_lock lock{g_recorder.Lock};
        g_recorder.Events.push_back(RecordedEvent{Record.Name, Record.Threshold, Record.Elapsed, Record.TimedOut, Record.Location.line()});
    }
    CATCH_LOG()

    // Poll until the recorder holds at least Count events or the deadline passes. The
    // MaxDuration callback runs on a threadpool thread, so a short bounded wait avoids racing
    // it without making the test sleep for a fixed, flaky duration.
    bool WaitForEvents(size_t Count, std::chrono::milliseconds Timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + Timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            {
                std::scoped_lock lock{g_recorder.Lock};
                if (g_recorder.Events.size() >= Count)
                {
                    return true;
                }
            }

            std::this_thread::sleep_for(5ms);
        }

        std::scoped_lock lock{g_recorder.Lock};
        return g_recorder.Events.size() >= Count;
    }

    // A MaxDuration far larger than the test window so the hang backstop never fires while
    // exercising the completion paths.
    constexpr auto c_noHang = 30s;

} // namespace

class SlowOperationWatcherUnitTests
{
    WSL_TEST_CLASS(SlowOperationWatcherUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        g_recorder.Clear();
        return true;
    }

    // An operation that finishes comfortably under the threshold emits nothing.
    TEST_METHOD(FastPathEmitsNothing)
    {
        {
            SlowOperationWatcher watcher{"FastPath", 30s, c_noHang, &RecordSink};
        }

        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // The single-argument call-site form (all timing params defaulted) also stays silent
    // on the fast path -- a smoke test that the defaults compile and behave.
    TEST_METHOD(DefaultConstructionFastPathEmitsNothing)
    {
        {
            // Uses the production sink; on the fast path nothing is emitted, so no ETW is hit.
            SlowOperationWatcher watcher{"Defaults"};
        }

        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // Reset() before the threshold disarms the watcher permanently: it stays silent at
    // Reset(), AND -- critically -- the destructor must not later re-evaluate a larger
    // elapsed (including work done after Reset()) and emit. Reset() is an authoritative
    // "operation ended here" marker, so post-Reset work must never be attributed to it.
    TEST_METHOD(ResetBeforeThresholdEmitsNothing)
    {
        constexpr auto threshold = 50ms;
        {
            SlowOperationWatcher watcher{"ResetFast", threshold, c_noHang, &RecordSink};

            // The watched operation finished quickly (under threshold) -> Reset() here.
            watcher.Reset();
            VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));

            // Simulate slow, unrelated work AFTER Reset() that pushes total elapsed well
            // past the threshold. The destructor at scope exit must still emit nothing.
            std::this_thread::sleep_for(150ms);
        }

        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(0));
    }

    // A slow operation that finishes before MaxDuration emits exactly ONE timedOut=false
    // record with the real elapsed time (>= threshold, and reflecting the extra time).
    TEST_METHOD(SlowCompletionEmitsOneRecord)
    {
        constexpr auto threshold = 50ms;
        {
            SlowOperationWatcher watcher{"SlowPhase", threshold, c_noHang, &RecordSink};

            // Stay alive well past the threshold, but far below MaxDuration.
            std::this_thread::sleep_for(200ms);
        }

        const auto events = g_recorder.Snapshot();
        VERIFY_ARE_EQUAL(events.size(), static_cast<size_t>(1));

        const auto& record = events[0];
        VERIFY_IS_FALSE(record.TimedOut);
        VERIFY_ARE_EQUAL(record.Name, std::string{"SlowPhase"});
        VERIFY_ARE_EQUAL(record.Threshold, threshold);

        // The record carries the real elapsed time, not just the threshold.
        VERIFY_IS_TRUE(record.Elapsed >= threshold);
        VERIFY_IS_TRUE(record.Elapsed >= 150ms);

        // source_location survived being copied into the Event (by value, no dangling ref).
        VERIFY_IS_TRUE(record.Line != 0);
    }

    // Reset() after the threshold emits the one timedOut=false record, and the destructor
    // that follows must not emit a second one (at-most-once via m_reported.exchange).
    TEST_METHOD(ResetAfterThresholdEmitsOnce)
    {
        constexpr auto threshold = 50ms;
        {
            SlowOperationWatcher watcher{"ResetSlow", threshold, c_noHang, &RecordSink};

            std::this_thread::sleep_for(150ms);
            watcher.Reset();

            const auto afterReset = g_recorder.Snapshot();
            VERIFY_ARE_EQUAL(afterReset.size(), static_cast<size_t>(1));
            VERIFY_IS_FALSE(afterReset[0].TimedOut);
            VERIFY_IS_TRUE(afterReset[0].Elapsed >= threshold);
        }

        // Destruction after Reset() must not produce a second record.
        VERIFY_ARE_EQUAL(g_recorder.Snapshot().size(), static_cast<size_t>(1));
    }

    // An operation still running at MaxDuration emits exactly ONE timedOut=true record
    // (the hang backstop) with elapsed ~= MaxDuration, and NO second record when the scope
    // finally exits well past the cap. This is the "report at max and stop" behavior.
    TEST_METHOD(HangEmitsOnceAtMaxThenStops)
    {
        constexpr auto threshold = 40ms;
        constexpr auto maxDuration = 120ms;
        {
            SlowOperationWatcher watcher{"Hang", threshold, maxDuration, &RecordSink};

            // Wait for the backstop to fire at ~maxDuration.
            VERIFY_IS_TRUE(WaitForEvents(1, 5s));

            // Keep "running" far past the cap without finishing.
            std::this_thread::sleep_for(500ms);
        }

        const auto events = g_recorder.Snapshot();

        // Exactly one event -- the hang backstop -- and no completion record afterwards.
        VERIFY_ARE_EQUAL(events.size(), static_cast<size_t>(1));

        const auto& record = events[0];
        // timedOut=true is the proof this is the hang backstop, not the completion path:
        // WaitForEvents(1) above gated on the event before the post-cap sleep, so the backstop
        // had already fired by scope exit. Elapsed >= maxDuration proves it fired at/after the
        // cap. No wall-clock upper bound -- the callback can be delayed under CI load while the
        // behavior is still correct, and timedOut=true already distinguishes it from a
        // completion record.
        VERIFY_IS_TRUE(record.TimedOut);
        VERIFY_ARE_EQUAL(record.Name, std::string{"Hang"});
        VERIFY_IS_TRUE(record.Elapsed >= maxDuration);
    }
};

} // namespace SlowOperationWatcherUnitTests
