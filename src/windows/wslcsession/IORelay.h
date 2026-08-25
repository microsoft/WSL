/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IORelay.h

Abstract:

    Contains the definition of the IORelay class.

--*/

#pragma once

namespace wsl::windows::service::wslc {

class IORelay
{

public:
    NON_COPYABLE(IORelay);

    IORelay();
    ~IORelay();

    void AddHandles(std::vector<std::unique_ptr<common::io::OverlappedIOHandle>>&& Handles);
    void AddHandle(std::unique_ptr<common::io::OverlappedIOHandle>&& Handle);

    void Stop();

    // Returns true if the calling thread is the IORelay's own worker thread (i.e. the call
    // is being made from a handle callback). Destroying the IORelay from this thread would
    // join the thread with itself and call std::terminate(), so callers that may run on the
    // relay thread must check this before destroying the object.
    bool IsRelayThread() const noexcept;

private:
    void Start();
    void Run();

    std::mutex m_pendingHandlesLock;
    wil::unique_event m_refreshEvent{wil::EventOptions::None};
    std::vector<std::unique_ptr<common::io::OverlappedIOHandle>> m_pendingHandles;

    std::thread m_thread;
    std::atomic<bool> m_exit = false;
};

} // namespace wsl::windows::service::wslc