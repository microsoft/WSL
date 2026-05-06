/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PortRelayHandle.h

Abstract:

    Contains the definition of the PortRelayAcceptHandle class for port relaying.

--*/

#pragma once

#include "relay.hpp"

namespace wsl::windows::service::wslc {

class IORelay;

class PortRelayAcceptHandle : public common::relay::OverlappedIOHandle
{
public:
    NON_COPYABLE(PortRelayAcceptHandle)
    NON_MOVABLE(PortRelayAcceptHandle)

    PortRelayAcceptHandle(wil::unique_socket&& ListenSocket, const GUID& VmId, uint32_t RelayPort, uint32_t LinuxPort, int Family, IORelay& IoRelay);

    ~PortRelayAcceptHandle();

    void Schedule() override;
    void Collect() override;
    HANDLE GetHandle() const override;

private:
    void LaunchRelay(wil::unique_socket&& AcceptedSocket);

    wil::unique_socket ListenSocket;
    wil::unique_socket AcceptedSocket;
    GUID VmId;
    uint32_t RelayPort;
    uint32_t LinuxPort;
    int Family;
    IORelay& IoRelay;
    wil::unique_event Event{wil::EventOptions::ManualReset};
    OVERLAPPED Overlapped{};
    char AcceptBuffer[2 * sizeof(SOCKADDR_STORAGE)]{};
};

} // namespace wsl::windows::service::wslc
