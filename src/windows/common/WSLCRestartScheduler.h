/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCRestartScheduler.h

Abstract:

    Contains the definition for WSLCRestartScheduler.

--*/

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace wsl::windows::service::wslc {

// Runs restart-policy callbacks on a dedicated thread once their delay elapses. Callbacks run one at a
// time, in due-time order, with no scheduler lock held.
class WSLCRestartScheduler
{
public:
    NON_COPYABLE(WSLCRestartScheduler);
    NON_MOVABLE(WSLCRestartScheduler);

    WSLCRestartScheduler() = default;
    ~WSLCRestartScheduler();

    // Ignored once Stop() has been called.
    void Schedule(std::chrono::milliseconds Delay, std::function<void()>&& Callback);

    // Discards pending callbacks and waits for a running callback to return. Must be called outside the
    // scheduler thread and without a lock that a callback can acquire.
    void Stop() noexcept;

private:
    void Run() noexcept;

    std::mutex m_lock;
    std::condition_variable m_wake;
    _Guarded_by_(m_lock) std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> m_queue;
    _Guarded_by_(m_lock) bool m_stopped = false;
    _Guarded_by_(m_lock) std::thread m_thread;
};

} // namespace wsl::windows::service::wslc
