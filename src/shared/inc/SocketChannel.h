/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SocketChannel.h

Abstract:

    This file contains the SocketChannel helper class implementation.

--*/

#pragma once

#include <mutex>
#include "socketshared.h"
#include "lxinitshared.h"

#ifndef WIN32
#include <assert.h>
#include "lxwil.h"
#include "../../linux/init/util.h"
extern std::optional<bool> g_EnableSocketLogging;
#endif

namespace wsl::shared {
#ifdef WIN32

using TSocket = wil::unique_socket;
using TTimeout = DWORD;

constexpr DWORD DefaultSocketTimeout = INFINITE;

#else

using TSocket = wil::unique_fd;
using TTimeout = const timeval*;
constexpr timeval* DefaultSocketTimeout = nullptr;

#endif

class SocketChannel
{

public:
    SocketChannel() = default;

    SocketChannel(const SocketChannel&) = delete;
    SocketChannel(SocketChannel&& other)
    {
        *this = std::move(other);
    }

    SocketChannel& operator=(const SocketChannel&) = delete;
    SocketChannel& operator=(SocketChannel&& other)
    {
        m_name = std::move(other.m_name);
        m_socket = std::move(other.m_socket);

#ifdef WIN32
        m_exitEvent = std::move(other.m_exitEvent);
#endif
        m_ignore_sequence = other.m_ignore_sequence;

        return *this;
    }

    // Note: 'name' must be a global string, since SocketChannel doesn't make a copy of it.
    SocketChannel(TSocket&& socket, const char* name) : m_socket(std::move(socket)), m_name(name)
    {
    }

#ifdef WIN32

    SocketChannel(TSocket&& socket, const char* name, HANDLE exitEvent) :
        m_socket(std::move(socket)), m_exitEvent(exitEvent), m_name(name)
    {
    }

#endif

    template <typename TMessage>
    void SendMessage(gsl::span<gsl::byte> span)
    {
        // Ensure that no other thread is using this channel.
        const std::unique_lock<std::mutex> lock{m_sendMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {

#ifdef WIN32

            THROW_HR_MSG(E_UNEXPECTED, "Incorrect channel usage detected on channel: %hs, message type: %hs", m_name, ToString(TMessage::Type));

#else

            LOG_ERROR("Incorrect channel usage detected on channel: {}, message type: {}", m_name, ToString(TMessage::Type));
            THROW_ERRNO(EINVAL);

#endif
        }

        THROW_INVALID_ARG_IF(m_name == nullptr || span.size() < sizeof(TMessage));

        m_sent_messages++;

        auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(span);
        WI_ASSERT(header->MessageSize == span.size());

        header->SequenceNumber = m_sent_messages;

#ifdef WIN32

        auto sentBytes = wsl::windows::common::socket::Send(m_socket.get(), span, m_exitEvent);

        WSL_LOG(
            "SentMessage",
            TraceLoggingValue(m_name, "Name"),
            TraceLoggingValue(reinterpret_cast<const TMessage*>(span.data())->PrettyPrint().c_str(), "Content"),
            TraceLoggingValue(sentBytes, "SentBytes"));

#else

        if (LoggingEnabled())
        {
            LOG_INFO("SentMessage on channel: {}: '{}'", m_name, reinterpret_cast<const TMessage*>(span.data())->PrettyPrint().c_str());
        }

        if (UtilWriteBuffer(m_socket.get(), span.data(), span.size()) < 0)
        {
            LOG_ERROR("Failed to write message {}. Channel: {}", header->MessageType, m_name);
            THROW_LAST_ERROR();
        }

#endif
    }

    template <typename TMessage>
    MESSAGE_HEADER& GetMessageHeader(TMessage& message)
    {
        if constexpr (std::is_same_v<TMessage, MESSAGE_HEADER>)
        {
            return message;
        }
        else
        {
            return message.Header;
        }
    }

    template <typename TMessage>
    void SendMessage(TMessage& message)
    {
        // Catch situations where the other SendMessage() method should be used
        const auto& header = GetMessageHeader(message);
        if (header.MessageSize != sizeof(message))
        {
#ifdef WIN32
            THROW_HR_MSG(E_INVALIDARG, "Incorrect header size for message type: %u on channel: %hs", header.MessageType, m_name);
#else
            LOG_ERROR("Incorrect header size for message type: {} on channel: {}", header.MessageType, m_name);
            THROW_ERRNO(EINVAL);
#endif
        }

        SendMessage<TMessage>(gslhelpers::struct_as_writeable_bytes(message));
    }

    template <typename TResult>
    void SendResultMessage(TResult value)
    {
        RESULT_MESSAGE<TResult> Result{};
        Result.Header.MessageSize = sizeof(Result);
        Result.Header.MessageType = RESULT_MESSAGE<TResult>::Type;
        Result.Result = value;

        SendMessage(Result);
    }

    template <typename TMessage>
    std::pair<TMessage*, gsl::span<gsl::byte>> ReceiveMessageOrClosed(TTimeout timeout = DefaultSocketTimeout)
    {
        WI_ASSERT(m_name != nullptr);

        // Ensure that no other thread is using this channel.
        const std::unique_lock<std::mutex> lock{m_receiveMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {

#ifdef WIN32

            THROW_HR_MSG(E_UNEXPECTED, "Incorrect channel usage detected on channel: %hs", m_name);
#else

            LOG_ERROR("Incorrect channel usage detected on channel: {}", m_name);
            THROW_ERRNO(EINVAL);

#endif
        }

        m_received_messages++;

        auto receivedSpan = ReceiveImpl(TMessage::Type, timeout);
        if (receivedSpan.empty())
        {

#ifdef WIN32
            if (errno == HCS_E_CONNECTION_TIMEOUT)
            {
                THROW_HR_MSG(HCS_E_CONNECTION_TIMEOUT, "Timeout: %d, expected type: %hs, channel: %hs", timeout, ToString(TMessage::Type), m_name);
            }
#endif

            return {nullptr, {}};
        }

        auto* message = gslhelpers::try_get_struct<TMessage>(receivedSpan);

        if (message == nullptr)
        {
#ifdef WIN32
            THROW_HR_MSG(
                E_UNEXPECTED, "Message size is too small: %zd, expected type: %hs, channel: %hs", receivedSpan.size(), ToString(TMessage::Type), m_name);
#else
            LOG_ERROR("MessageSize is too small: {}, expected type: {}, channel: {}", receivedSpan.size(), ToString(TMessage::Type), m_name);
            THROW_ERRNO(EINVAL);
#endif
        }

        ValidateMessageHeader(GetMessageHeader(*message), TMessage::Type, m_received_messages);

#ifdef WIN32
        WSL_LOG(
            "ReceivedMessage", TraceLoggingValue(m_name, "Name"), TraceLoggingValue(message->PrettyPrint().c_str(), "Content"));
#else
        if (LoggingEnabled())
        {
            LOG_INFO("ReceivedMessage on channel: {}: '{}'", m_name, message->PrettyPrint().c_str());
        }
#endif
        return {message, receivedSpan};
    }

    template <typename TMessage>
    TMessage& ReceiveMessage(gsl::span<gsl::byte>* responseSpan = nullptr, TTimeout timeout = DefaultSocketTimeout)
    {
        auto [message, span] = ReceiveMessageOrClosed<TMessage>(timeout);
        if (message == nullptr)
        {
#ifdef WIN32
            THROW_HR_MSG(E_UNEXPECTED, "Expected message %hs, but socket %hs was closed", ToString(TMessage::Type), m_name);
#else
            LOG_ERROR("ExpectedMessage {}, but socket {} was closed", ToString(TMessage::Type), m_name);
            THROW_ERRNO(EINVAL);
#endif
        }

        if (responseSpan != nullptr)
        {
            *responseSpan = span;
        }

        return *message;
    }

    template <typename TSentMessage>
    TSentMessage::TResponse& Transaction(gsl::span<gsl::byte> message, gsl::span<gsl::byte>* responseSpan = nullptr, TTimeout timeout = DefaultSocketTimeout)
    {
        SendMessage<TSentMessage>(message);

        return ReceiveMessage<typename TSentMessage::TResponse>(responseSpan, timeout);
    }

    template <typename TSentMessage>
    TSentMessage::TResponse& Transaction(TSentMessage& message, gsl::span<gsl::byte>* responseSpan = nullptr, TTimeout timeout = DefaultSocketTimeout)
    {
        WI_ASSERT(message.Header.MessageSize == sizeof(message));

        return Transaction<TSentMessage>(gslhelpers::struct_as_writeable_bytes(message), responseSpan, timeout);
    }

    void Close()
    {
        m_socket.reset();
    }

    auto Socket() const
    {
        return m_socket.get();
    }

    void IgnoreSequenceNumbers()
    {
        m_ignore_sequence = true;
    }

#ifndef WIN32

    static void EnableSocketLogging(bool enable)
    {
        g_EnableSocketLogging = enable;
    }

#endif

private:
#ifdef WIN32

    gsl::span<gsl::byte> ReceiveImpl(auto expectedMessage, TTimeout timeout)
    {
        return wsl::shared::socket::RecvMessage(m_socket.get(), m_buffer, m_exitEvent, timeout);
    }

#else

    gsl::span<gsl::byte> ReceiveImpl(auto expectedMessage, TTimeout timeout)
    {
        return wsl::shared::socket::RecvMessage(m_socket.get(), m_buffer, timeout);
    }

#endif

    void ValidateMessageHeader(const MESSAGE_HEADER& header, LX_MESSAGE_TYPE expected, unsigned int expectedSequence) const
    {
        if (header.MessageSize < sizeof(header) || (expected != LxMiniInitMessageAny && header.MessageType != expected) ||
            (!m_ignore_sequence && header.SequenceNumber != expectedSequence))
        {
#ifdef WIN32

            THROW_HR_MSG(
                E_UNEXPECTED,
                "Protocol error: Received message size: %u, type: %u, sequence: %u. Expected type: %u, expected sequence: %u, "
                "channel: %hs",
                header.MessageSize,
                header.MessageType,
                header.SequenceNumber,
                expected,
                expectedSequence,
                m_name);
#else

            LOG_ERROR(
                "Protocol error: Received message size: {}, type: {}, sequence: {}. Expected type: {}, expected sequence: {}, "
                "channel: %s",
                header.MessageSize,
                header.MessageType,
                header.SequenceNumber,
                expected,
                expectedSequence,
                m_name);

            THROW_ERRNO(EINVAL);

#endif
        }
    }

#ifndef WIN32

    static bool LoggingEnabled()
    {
        static std::once_flag flag;
        std::call_once(flag, [&]() {
            try
            {
                if (g_EnableSocketLogging.has_value())
                {
                    return;
                }

                auto content = UtilReadFileContent("/proc/cmdline");
                g_EnableSocketLogging = content.find("WSL_SOCKET_LOG") != std::string::npos;
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                g_EnableSocketLogging = false;
            }
        });

        return g_EnableSocketLogging.value();
    }

#endif

    TSocket m_socket{};
    std::vector<gsl::byte> m_buffer;

#ifdef WIN32

    HANDLE m_exitEvent{};

#endif
    uint32_t m_sent_messages = 0;
    uint32_t m_received_messages = 0;
    bool m_ignore_sequence = false;
    const char* m_name{};
    std::mutex m_sendMutex;
    std::mutex m_receiveMutex;
};
} // namespace wsl::shared