/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessIO.h

Abstract:

    Contains the different WSLAProcessIO definitions for process IO handling.

--*/

#pragma once
#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class WSLAProcessIO
{
public:
    virtual ~WSLAProcessIO() = default;
    virtual wil::unique_handle OpenFd(ULONG Fd) = 0;
};

class RelayedProcessIO : public WSLAProcessIO
{
public:
    RelayedProcessIO(wil::unique_handle&& IoStream);
    virtual ~RelayedProcessIO();

    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    void RunIORelay(wil::unique_hfile&& stdinPipe, wil::unique_hfile&& stdoutPipe, wil::unique_hfile&& stderrPipe);
    void StartIORelay();

    std::thread m_thread;
    wil::unique_handle m_ioStream;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    std::optional<std::map<ULONG, wil::unique_handle>> m_relayedHandles;
};

class TTYProcessIO : public WSLAProcessIO
{
public:
    TTYProcessIO(wil::unique_handle&& IoStream);

    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    wil::unique_handle m_ioStream;
};

class VMProcessIO : public WSLAProcessIO
{
public:
    VMProcessIO(std::map<ULONG, wil::unique_handle>&& handles);
    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, wil::unique_handle> m_handles;
};

} // namespace wsl::windows::service::wsla