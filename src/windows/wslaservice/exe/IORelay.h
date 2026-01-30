/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IORelay.h

Abstract:

    Contains the definition of the IORelay class.

--*/

#pragma once

namespace wsl::windows::service::wsla {

class IORelay
{

public:
    NON_COPYABLE(IORelay);

    IORelay();
    ~IORelay();

    void AddHandles(std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>>&& Handles);
    void AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle);

    void Stop();

private:
    void Start();
    void Run();

    std::mutex m_pendingHandlesLock;
    wil::unique_event m_refreshEvent{wil::EventOptions::None};
    std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>> m_pendingHandles;

    std::thread m_thread;
    bool m_exit = false;
};

} // namespace wsl::windows::service::wsla