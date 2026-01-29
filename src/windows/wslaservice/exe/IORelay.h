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

    IORelay() = default;
    ~IORelay();

    void AddHandles(std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>>&& Handles);
    void AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle);

private:
    void StartRelayThread();
    void StopRelayThread();
    void Run();

    std::thread m_thread;
    windows::common::relay::MultiHandleWait m_io;
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
};

} // namespace wsl::windows::service::wsla