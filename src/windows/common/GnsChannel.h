// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <wil/resource.h>
#include "SocketChannel.h"
#include "hns_schema.h"

namespace wsl::core {

// This class manages the hvsocket channel between wslservice and the gns process inside the VM.
// This channel is used for network configuration inside the guest.
class GnsChannel
{
public:
    GnsChannel(wil::unique_socket&& Socket);

    GnsChannel(const GnsChannel&) = delete;

    GnsChannel& operator=(const GnsChannel&) = delete;

    GnsChannel(GnsChannel&&) = default;

    GnsChannel& operator=(GnsChannel&&) = default;

    void SendEndpointState(const wsl::shared::hns::HNSEndpoint& Notification);

    void SendHnsNotification(LPCWSTR Notification, const GUID& AdapterId);

    void SendNetworkDeviceMessage(LX_MESSAGE_TYPE MessageType, LPCWSTR MessageContent);
    int SendNetworkDeviceMessageReturnResult(LX_MESSAGE_TYPE MessageType, LPCWSTR MessageContent);

    void Stop() const;

private:
    template <typename T>
    int MessageReturnResult(LX_MESSAGE_TYPE Type, const std::string& Content, const std::function<void(T&)>& BuildMessage = [](auto&) {});

    template <typename T>
    void Message(LX_MESSAGE_TYPE Type, const std::string& Content, const std::function<void(T&)>& BuildMessage = [](auto&) {});

    // m_channel depends on m_stopEvent, so m_channel needs to be destroyed first.
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
    wsl::shared::SocketChannel m_channel;
};
} // namespace wsl::core
