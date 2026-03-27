/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessIO.h

Abstract:

    Contains the different WSLCProcessIO definitions for process IO handling.

--*/

#pragma once
#include "wslc.h"

namespace wsl::windows::service::wslc {

class WSLCProcessIO
{
public:
    virtual ~WSLCProcessIO() = default;
    virtual wil::unique_handle OpenFd(ULONG Fd) = 0;
};

class RelayedProcessIO : public WSLCProcessIO
{
public:
    RelayedProcessIO(std::map<ULONG, wil::unique_handle>&& fds);

    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, wil::unique_handle> m_relayedHandles;
};

class TTYProcessIO : public WSLCProcessIO
{
public:
    TTYProcessIO(wil::unique_handle&& IoStream);

    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    wil::unique_handle m_ioStream;
};

class VMProcessIO : public WSLCProcessIO
{
public:
    VMProcessIO(std::map<ULONG, wil::unique_handle>&& handles);
    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, wil::unique_handle> m_handles;
};

} // namespace wsl::windows::service::wslc