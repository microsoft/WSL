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
    RelayedProcessIO(std::map<ULONG, wil::unique_handle>&& fds);

    wil::unique_handle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, wil::unique_handle> m_relayedHandles;
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