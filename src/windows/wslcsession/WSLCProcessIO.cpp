/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessIO.cpp

Abstract:

    Contains the different WSLCProcessIO implementations for process IO handling.

--*/

#include "precomp.h"

#include "WSLCProcessIO.h"

using wsl::windows::service::wslc::RelayedProcessIO;
using wsl::windows::service::wslc::TTYProcessIO;
using wsl::windows::service::wslc::TypedHandle;
using wsl::windows::service::wslc::VMProcessIO;
using namespace wsl::windows::common::relay;

RelayedProcessIO::RelayedProcessIO(std::map<ULONG, TypedHandle>&& fds) : m_relayedHandles(std::move(fds))
{
}

TypedHandle RelayedProcessIO::OpenFd(ULONG Fd)
{
    auto it = m_relayedHandles.find(Fd);

    THROW_HR_IF_MSG(E_INVALIDARG, it == m_relayedHandles.end(), "Fd not found in relayed handles: %i", static_cast<int>(Fd));
    THROW_WIN32_IF_MSG(ERROR_INVALID_STATE, !it->second.is_valid(), "Fd already consumed: %i", static_cast<int>(Fd));

    return std::move(it->second);
}

TTYProcessIO::TTYProcessIO(TypedHandle&& IoStream) : m_ioStream(std::move(IoStream))
{
}

TypedHandle TTYProcessIO::OpenFd(ULONG Fd)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Fd != WSLCFDTty, "Invalid fd type for TTY process: %i", static_cast<int>(Fd));

    return std::move(m_ioStream);
}

VMProcessIO::VMProcessIO(std::map<ULONG, TypedHandle>&& handles) : m_handles(std::move(handles))
{
}

TypedHandle VMProcessIO::OpenFd(ULONG Fd)
{
    auto it = m_handles.find(Fd);
    THROW_HR_IF_MSG(E_INVALIDARG, it == m_handles.end(), "Invalid fd type for VM process: %i", static_cast<int>(Fd));

    return std::move(it->second);
}