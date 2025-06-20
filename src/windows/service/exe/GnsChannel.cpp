// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "lxinitshared.h"
#include "GnsChannel.h"

using namespace wsl::shared;
using wsl::core::GnsChannel;

GnsChannel::GnsChannel(wil::unique_socket&& socket) : m_channel(std::move(socket), "GNS", m_stopEvent.get())
{
    WSL_LOG("GnsChannel::GnsChannel", TraceLoggingValue(m_channel.Socket(), "socket"));
}

void GnsChannel::SendEndpointState(const hns::HNSEndpoint& Notification)
{
    // if we have signaled to stop, block anyone making new calls
    if (m_stopEvent.is_signaled())
    {
        return;
    }

    Message<LX_GNS_INTERFACE_CONFIGURATION>(LxGnsMessageInterfaceConfiguration, ToJson(Notification));
}

template <typename TMessage>
int GnsChannel::MessageReturnResult(LX_MESSAGE_TYPE Type, const std::string& Content, const std::function<void(TMessage&)>& BuildMessage)
{
    size_t messageSize;
    THROW_IF_FAILED(SizeTAdd(offsetof(TMessage, Content), Content.size() + 1, &messageSize));

    // Populate the message that will be sent to gns.
    std::vector<gsl::byte> buffer(messageSize);
    const auto messageSpan = gsl::make_span(buffer);
    auto* message = gslhelpers::get_struct<TMessage>(messageSpan);
    message->Header.MessageType = Type;
    message->Header.MessageSize = gsl::narrow_cast<ULONG>(messageSize);
    if (BuildMessage)
    {
        BuildMessage(*message);
    }

    auto offset = offsetof(TMessage, Content);
    wsl::shared::string::CopyToSpan(Content, messageSpan, offset);
    WI_ASSERT(messageSize == offset);

    return m_channel.Transaction<TMessage>(messageSpan).Result;
}

template <typename TMessage>
void GnsChannel::Message(LX_MESSAGE_TYPE Type, const std::string& Content, const std::function<void(TMessage&)>& BuildMessage)
{
    const auto result = MessageReturnResult(Type, Content, BuildMessage);

    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        result == ERROR_FATAL_APP_EXIT,
        "Did not receive a LX_GNS_RESULT after sending message %hs, type %u",
        Content.c_str(),
        static_cast<uint32_t>(Type));

    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        (result != 0),
        "Error returned from GNS after sending message %hs, type %u. Result=%i",
        Content.c_str(),
        static_cast<uint32_t>(Type),
        result);
}

// the payload is expected to be of type ModifyGuestEndpointSettingRequest
void GnsChannel::SendHnsNotification(_In_ LPCWSTR Notification, const GUID& AdapterId)
{
    // if we have signaled to stop, block anyone making new calls
    if (m_stopEvent.is_signaled())
    {
        return;
    }

    auto AddAdapterId = [&](LX_GNS_NOTIFICATION& Message) { Message.AdapterId = AdapterId; };
    Message<LX_GNS_NOTIFICATION>(LxGnsMessageNotification, wsl::shared::string::WideToMultiByte(Notification), AddAdapterId);
}

// Network device messages built from the corresponding serialization functions
// throws on error
void GnsChannel::SendNetworkDeviceMessage(LX_MESSAGE_TYPE MessageType, LPCWSTR MessageContent)
{
    // if we have signaled to stop, block anyone making new calls
    if (m_stopEvent.is_signaled())
    {
        return;
    }

    WI_ASSERT(
        MessageType == LxGnsMessageVmNicCreatedNotification || MessageType == LxGnsMessageCreateDeviceRequest ||
        MessageType == LxGnsMessageModifyGuestDeviceSettingRequest || MessageType == LxGnsMessageLoopbackRoutesRequest ||
        MessageType == LxGnsMessageDeviceSettingRequest || MessageType == LxGnsMessageInitialIpConfigurationNotification ||
        MessageType == LxGnsMessageSetupIpv6 || MessageType == LxGnsMessageInterfaceConfiguration ||
        MessageType == LxGnsMessageNoOp || MessageType == LxGnsMessageGlobalNetFilter ||
        MessageType == LxGnsMessageInterfaceNetFilter || MessageType == LxGnsMessageConnectTestRequest);

    Message<LX_GNS_JSON_MESSAGE>(MessageType, wsl::shared::string::WideToMultiByte(MessageContent));
}

// Network device messages built from the corresponding serialization functions
// throws on error, returns the integer value that was returned from Linux
int GnsChannel::SendNetworkDeviceMessageReturnResult(LX_MESSAGE_TYPE MessageType, LPCWSTR MessageContent)
{
    // if we have signaled to stop, block anyone making new calls
    if (m_stopEvent.is_signaled())
    {
        return ERROR_SHUTDOWN_IN_PROGRESS;
    }

    WI_ASSERT(
        MessageType == LxGnsMessageVmNicCreatedNotification || MessageType == LxGnsMessageCreateDeviceRequest ||
        MessageType == LxGnsMessageModifyGuestDeviceSettingRequest || MessageType == LxGnsMessageLoopbackRoutesRequest ||
        MessageType == LxGnsMessageDeviceSettingRequest || MessageType == LxGnsMessageInitialIpConfigurationNotification ||
        MessageType == LxGnsMessageSetupIpv6 || MessageType == LxGnsMessageInterfaceConfiguration ||
        MessageType == LxGnsMessageNoOp || MessageType == LxGnsMessageGlobalNetFilter ||
        MessageType == LxGnsMessageInterfaceNetFilter || MessageType == LxGnsMessageConnectTestRequest);

    return MessageReturnResult<LX_GNS_JSON_MESSAGE>(MessageType, wsl::shared::string::WideToMultiByte(MessageContent));
}

void GnsChannel::Stop() const
{
    WSL_LOG("GnsChannel::Stop");
    m_stopEvent.SetEvent();
}
