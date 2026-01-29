/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessIO.cpp

Abstract:

    Contains the different WSLAProcessIO implementations for process IO handling.

--*/

#include "precomp.h"

#include "WSLAProcessIO.h"

using wsl::windows::service::wsla::RelayedProcessIO;
using wsl::windows::service::wsla::TTYProcessIO;
using wsl::windows::service::wsla::VMProcessIO;
using namespace wsl::windows::common::relay;

RelayedProcessIO::RelayedProcessIO(std::map<ULONG, wil::unique_handle>&& fds) : m_relayedHandles(std::move(fds))
{
}


wil::unique_handle RelayedProcessIO::OpenFd(ULONG Fd)
{
    auto it = m_relayedHandles.find(Fd);

    THROW_HR_IF_MSG(E_INVALIDARG, it == m_relayedHandles.end(), "Fd not found in relayed handles: %i", static_cast<int>(Fd));
    THROW_WIN32_IF_MSG(ERROR_INVALID_STATE, !it->second.is_valid(), "Fd already consumed: %i", static_cast<int>(Fd));

    return std::move(it->second);
}

TTYProcessIO::TTYProcessIO(wil::unique_handle&& IoStream) : m_ioStream(std::move(IoStream))
{
}

wil::unique_handle TTYProcessIO::OpenFd(ULONG Fd)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Fd != WSLAFDTty, "Invalid fd type for TTY process: %i", static_cast<int>(Fd));

    return std::move(m_ioStream);
}

VMProcessIO::VMProcessIO(std::map<ULONG, wil::unique_handle>&& handles) : m_handles(std::move(handles))
{
}

wil::unique_handle VMProcessIO::OpenFd(ULONG Fd)
{
    auto it = m_handles.find(Fd);
    THROW_HR_IF_MSG(E_INVALIDARG, it == m_handles.end(), "Invalid fd type for VM process: %i", static_cast<int>(Fd));

    return std::move(it->second);
}