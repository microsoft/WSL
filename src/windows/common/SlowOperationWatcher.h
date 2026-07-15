/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SlowOperationWatcher.h

Abstract:

    RAII guard that times a scoped operation and emits **at most one** `SlowOperation`
    telemetry event. There are exactly three outcomes:

      1. The operation finishes in under `SlowThreshold` (10 s default) -- the common,
         healthy case. Nothing is emitted.
      2. The operation finishes but took at least `SlowThreshold`. On scope exit
         (destructor or Reset()) one `timedOut=false` event is emitted carrying the real
         `elapsedMs` measured from construction -- the total time the scope was alive.
         Note this says nothing about whether the operation *succeeded*: a scope that
         exits by throwing is still reported here (the watcher is a pure duration timer
         and has no visibility into the operation's result).
      3. The operation is still running after `MaxDuration` (15 min default) -- i.e. it is
         hung or pathologically slow. A single-shot timer fires once at `MaxDuration` and
         emits one `timedOut=true` event with `elapsedMs ~= MaxDuration`, then stops.
         This is the backstop for an operation that never returns (e.g. an HCS wait that
         blocks INFINITE, where the destructor never runs). It never repeats, and once it
         has fired the later scope exit does not emit a second event. MaxDuration is the
         time a hang stays silent, so the default sits well above the longest legitimate
         operation timeout (the service's hard timeouts are on the order of minutes) to
         avoid misreporting a slow-but-valid operation, while still surfacing a true hang.

    So `SlowThreshold` is the "slow enough to be worth reporting" filter applied at
    completion, and `MaxDuration` is the "give up waiting and report the hang" deadline.
    Every event carries the phase name and the call site (std::source_location). The
    watcher is a passive observer: it never affects the operation it measures, and a
    failure in the telemetry path can never crash or slow the watched code.

    Usage:

        SlowOperationWatcher slow{"WaitForMiniInitConnect"};
        m_miniInitChannel = wsl::shared::SocketChannel{AcceptConnection(timeout), ...};

    If the scope needs to outlive the watched operation (for example to keep a
    pointer into an internal receive buffer alive without a nested block), call
    Reset() to disarm the watcher early:

        SlowOperationWatcher slow{"WaitForCreateInstanceResult"};
        const auto& result = channel.ReceiveMessage<...>(...);
        slow.Reset();
        // result remains valid and usable here

--*/

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <atomic>
#include <chrono>
#include <source_location>

class SlowOperationWatcher
{
public:
    // Contents of one SlowOperation telemetry record. Exposed so tests can observe
    // emissions through a custom sink without standing up an ETW listener.
    struct Event
    {
        const char* Name;                    // phase identifier; must outlive the watcher
        std::chrono::milliseconds Threshold; // configured slow threshold
        std::chrono::milliseconds Elapsed;   // real time since construction
        bool TimedOut;                       // false: the scope finished (slow); Elapsed is the total.
                                             // true:  still running at MaxDuration (hang backstop).
                                             // NOT a success/failure flag -- the watcher cannot know
                                             // the operation's result; a throwing scope is TimedOut=false.
        // By value, not a reference: a diagnostic sink may copy the Event and read it after
        // the watcher is destroyed, which would dangle a reference. std::source_location is
        // cheap to copy, so keep Event self-contained.
        std::source_location Location;
    };

    // Receives the (at most one) SlowOperation record. Must be noexcept and must not block:
    // it is invoked either from the MaxDuration threadpool callback (timedOut=true) or from
    // Reset()/the destructor (timedOut=false). The default writes the telemetry event.
    using Sink = void (*)(const Event&) noexcept;

    // Name is taken as a char-array reference (const char (&)[N]) rather than a const char*
    // to force callers to pass an array and block the easy UAF of a temporary's pointer
    // (e.g. std::string::c_str()): the raw pointer is dereferenced later from a threadpool
    // callback, so the pointed-to storage must outlive the watcher and any pending callback.
    // The array type does not by itself guarantee static storage -- a non-static array would
    // also compile -- so in practice always pass a string literal. Keep Name a short CamelCase
    // phase identifier that the backend query can switch on (e.g. "WaitForMiniInitConnect").
    // SlowThreshold is the "slow enough to report at completion" filter; MaxDuration is the
    // "report the hang and stop" deadline. OnSlow defaults to the telemetry emitter; tests
    // inject a recording sink.
    template <size_t N>
    explicit SlowOperationWatcher(
        const char (&Name)[N],
        std::chrono::milliseconds SlowThreshold = std::chrono::seconds{10},
        std::chrono::milliseconds MaxDuration = std::chrono::minutes{15},
        Sink OnSlow = &EmitTelemetry,
        std::source_location Location = std::source_location::current()) noexcept :
        SlowOperationWatcher(static_cast<const char*>(Name), SlowThreshold, MaxDuration, OnSlow, Location)
    {
    }

    // On destruction, emits the timedOut=false record if the operation was slow (>= threshold)
    // and the hang backstop hasn't already reported.
    ~SlowOperationWatcher() noexcept;

    // Disarm the watcher early (equivalent to the destructor, but at an explicit point). Cancels
    // and drains the MaxDuration timer, then -- if the operation took at least SlowThreshold and
    // the hang backstop hasn't already fired -- emits one timedOut=false record with the real
    // elapsed time. The fast path (under threshold) stays silent. Emits at most once across
    // Reset() + the destructor.
    void Reset() noexcept;

    SlowOperationWatcher(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher& operator=(const SlowOperationWatcher&) = delete;
    SlowOperationWatcher(SlowOperationWatcher&&) = delete;
    SlowOperationWatcher& operator=(SlowOperationWatcher&&) = delete;

private:
    explicit SlowOperationWatcher(
        _In_z_ const char* Name, std::chrono::milliseconds SlowThreshold, std::chrono::milliseconds MaxDuration, Sink OnSlow, std::source_location Location) noexcept;

    static void CALLBACK OnTimerFired(PTP_CALLBACK_INSTANCE, PVOID Context, PTP_TIMER) noexcept;

    // Default sink: writes the SlowOperation telemetry event.
    static void EmitTelemetry(const Event& Record) noexcept;

    std::chrono::milliseconds Elapsed() const noexcept;
    void Emit(bool TimedOut, std::chrono::milliseconds Elapsed) noexcept;

    // Cancel + drain the timer, then emit the timedOut=false record if the operation was slow
    // and the hang backstop hasn't already reported.
    void Finish() noexcept;

    const char* const m_name;
    const std::chrono::milliseconds m_slowThreshold;
    const std::chrono::milliseconds m_maxDuration;
    const std::source_location m_location;
    const std::chrono::steady_clock::time_point m_start;
    const Sink m_sink;
    std::atomic<bool> m_reported; // set once the single event has been emitted (by either path)
    wil::unique_threadpool_timer m_timer;
};
