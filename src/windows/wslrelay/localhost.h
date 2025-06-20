/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    localhost.h

Abstract:

    This file contains localhost port relay function declarations.

--*/

#pragma once

#include <winsock2.h>
#include <map>
#include <string_view>
#include <tuple>
#include "SocketChannel.h"

typedef struct _LX_PORT_LISTENER_THREAD_CONTEXT
{
    GUID VmId;
    unsigned short Family;
    unsigned short Port;
    int HvSocketPort;
    int Count = 1;
    wil::unique_event ExitEvent;
    wil::unique_socket ListenSocket;
} LX_PORT_LISTENER_THREAD_CONTEXT, *PLX_PORT_LISTENER_THREAD_CONTEXT;

typedef struct _LX_PORT_LISTENER_CONTEXT
{
    ~_LX_PORT_LISTENER_CONTEXT()
    {
        if (Worker.joinable())
        {
            Worker.join();
        }
    };
    std::shared_ptr<LX_PORT_LISTENER_THREAD_CONTEXT> ThreadContext;
    std::thread Worker;
} LX_PORT_LISTENER_CONTEXT, *PLX_PORT_LISTENER_CONTEXT;

namespace wsl::windows::wslrelay::localhost {
void RelayWorker(_In_ wsl::shared::SocketChannel& SocketChannel, _In_ const GUID& VmId);

class Relay
{
public:
    ~Relay();

    int StartPortListener(_In_ const GUID& VmId, _In_ unsigned short Family, _In_ unsigned short Port, _In_ int HvSocketPort);

    void StopPortListener(_In_ unsigned short Family, _In_ unsigned short Port);

private:
    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::map<std::tuple<unsigned short, unsigned short>, std::shared_ptr<LX_PORT_LISTENER_CONTEXT>> m_RelayThreads;
};
} // namespace wsl::windows::wslrelay::localhost
