/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessIO.h

Abstract:

    Contains the different WSLCProcessIO definitions for process IO handling.

--*/

#pragma once
#include "HandleIO.h"
#include "wslc.h"

namespace wsl::windows::service::wslc {

struct TypedHandle
{
    common::io::HandleWrapper Handle;
    WSLCHandleType Type = WSLCHandleTypeUnknown;

    TypedHandle() = default;
    TypedHandle(common::io::HandleWrapper&& handle, WSLCHandleType type) : Handle(std::move(handle)), Type(type)
    {
    }

    bool is_valid() const noexcept
    {
        return Handle.IsValid();
    }
    HANDLE get() const noexcept
    {
        return Handle.Get();
    }
};

class WSLCProcessIO
{
public:
    virtual ~WSLCProcessIO() = default;
    virtual TypedHandle OpenFd(ULONG Fd) = 0;
};

class RelayedProcessIO : public WSLCProcessIO
{
public:
    RelayedProcessIO(std::map<ULONG, TypedHandle>&& fds);

    TypedHandle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, TypedHandle> m_relayedHandles;
};

class TTYProcessIO : public WSLCProcessIO
{
public:
    TTYProcessIO(TypedHandle&& IoStream);

    TypedHandle OpenFd(ULONG Fd) override;

private:
    TypedHandle m_ioStream;
};

class VMProcessIO : public WSLCProcessIO
{
public:
    VMProcessIO(std::map<ULONG, TypedHandle>&& handles);
    TypedHandle OpenFd(ULONG Fd) override;

private:
    std::map<ULONG, TypedHandle> m_handles;
};

} // namespace wsl::windows::service::wslc