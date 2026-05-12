/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SocketChannel.h

Abstract:

    This file contains the SocketChannel helper class implementation.

--*/

#pragma once

#include <atomic>
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

class SocketChannel;

class Transaction
{
    friend class SocketChannel;

public:
    ~Transaction() = default;

    NON_COPYABLE(Transaction);

    template <typename TMessage>
    void Send(gsl::span<gsl::byte> span);

    template <typename TMessage>
    void Send(TMessage& message);

    template <typename TResult>
    void SendResultMessage(TResult value);

    template <typename TMessage>
    std::pair<TMessage*, gsl::span<gsl::byte>> ReceiveOrClosed();

    template <typename TMessage>
    TMessage& Receive(gsl::span<gsl::byte>* responseSpan = nullptr);

private:
    Transaction(SocketChannel& channel, uint32_t id, TTimeout timeout) :
        m_channel(channel), m_id(id), m_step(static_cast<uint32_t>(TRANSACTION_STEP::REQUEST)), m_deadline(ComputeDeadline(timeout))
    {
    }

    TTimeout RemainingTimeout() const;

    static std::optional<std::chrono::steady_clock::time_point> ComputeDeadline(TTimeout timeout);

    SocketChannel& m_channel;
    uint32_t m_id;
    /** Use uint32_t as step can go beyond FIRST_REPLY */
    uint32_t m_step;
    std::optional<std::chrono::steady_clock::time_point> m_deadline;

#ifndef WIN32
    // This is required because the Linux timeout logic requires a pointer. Returning a pointer is OK because only thread can use a transaction at a given time.
    mutable timeval m_timeoutStorage{};
#endif
};

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
        m_exitEvents = std::move(other.m_exitEvents);
#endif
        m_ignore_sequence = other.m_ignore_sequence;
        m_sent_non_transaction_messages = other.m_sent_non_transaction_messages;
        m_received_non_transaction_messages = other.m_received_non_transaction_messages;
        m_transaction_id_seed = other.m_transaction_id_seed.load();

        return *this;
    }

    SocketChannel(TSocket&& socket, std::string&& name) : m_socket(std::move(socket)), m_name(std::move(name))
    {
    }

#ifdef WIN32

    SocketChannel(TSocket&& socket, std::string&& name, std::vector<HANDLE>&& exitEvents) :
        m_socket(std::move(socket)), m_exitEvents(std::move(exitEvents)), m_name(std::move(name))
    {
    }

    std::vector<HANDLE> SetExitEvents(std::vector<HANDLE>&& exitEvents)
    {
        std::vector<HANDLE> oldEvents;
        std::swap(oldEvents, m_exitEvents);
        return oldEvents;
    }

    const std::vector<HANDLE>& GetExitEvents() const
    {
        return m_exitEvents;
    }

#endif

    template <typename TMessage>
    void SendMessage(gsl::span<gsl::byte> span, uint32_t transactionStep = static_cast<uint32_t>(TRANSACTION_STEP::NONE), uint32_t transactionId = 0, TTimeout timeout = DefaultSocketTimeout)
    {
        // Ensure that no other thread is using this channel.
        const std::unique_lock<std::mutex> lock{m_sendMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {

#ifdef WIN32

            THROW_HR_MSG(E_UNEXPECTED, "Incorrect channel usage detected on channel: %hs, message type: %hs", m_name.c_str(), ToString(TMessage::Type));

#else

            LOG_ERROR("Incorrect channel usage detected on channel: {}, message type: {}", m_name, ToString(TMessage::Type));
            THROW_ERRNO(EINVAL);

#endif
        }

        THROW_INVALID_ARG_IF(m_name.empty() || span.size() < sizeof(TMessage));

        auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(span);
        WI_ASSERT(header->MessageSize == span.size());

        if (transactionStep == static_cast<unsigned int>(TRANSACTION_STEP::NONE))
        {
            m_sent_non_transaction_messages++;
            header->TransactionId = m_sent_non_transaction_messages;
            header->TransactionStep = static_cast<unsigned int>(TRANSACTION_STEP::NONE);
        }
        else
        {
            header->TransactionId = transactionId;
            header->TransactionStep = transactionStep;
        }

#ifdef WIN32

        auto io = CreateIO();
        io.AddHandle(std::make_unique<windows::common::relay::WriteHandle>(m_socket.get(), span));

        io.Run(TimeoutToMilliseconds(timeout));

        WSL_LOG(
            "SentMessage",
            TraceLoggingValue(m_name.c_str(), "Name"),
            TraceLoggingValue(reinterpret_cast<const TMessage*>(span.data())->PrettyPrint().c_str(), "Content"));

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
    void SendMessage()
    {
        TMessage message;
        SendMessage(message);
    }

    template <typename TMessage>
    void SendMessage(TMessage& message, uint32_t transactionStep = static_cast<uint32_t>(TRANSACTION_STEP::NONE), uint32_t transactionId = 0, TTimeout timeout = DefaultSocketTimeout)
    {
        // Catch situations where the other SendMessage() method should be used
        const auto& header = GetMessageHeader(message);
        if (header.MessageSize != sizeof(message))
        {
#ifdef WIN32
            THROW_HR_MSG(E_INVALIDARG, "Incorrect header size for message type: %u on channel: %hs", header.MessageType, m_name.c_str());
#else
            LOG_ERROR("Incorrect header size for message type: {} on channel: {}", header.MessageType, m_name);
            THROW_ERRNO(EINVAL);
#endif
        }

        SendMessage<TMessage>(gslhelpers::struct_as_writeable_bytes(message), transactionStep, transactionId, timeout);
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
    std::pair<TMessage*, gsl::span<gsl::byte>> ReceiveMessageOrClosed(
        TTimeout timeout = DefaultSocketTimeout,
        uint32_t expectedTransactionStep = static_cast<uint32_t>(TRANSACTION_STEP::NONE),
        uint32_t expectedTransactionId = 0)
    {
        WI_ASSERT(!m_name.empty());

        // Ensure that no other thread is using this channel.
        const std::unique_lock<std::mutex> lock{m_receiveMutex, std::try_to_lock};
        if (!lock.owns_lock())
        {

#ifdef WIN32

            THROW_HR_MSG(E_UNEXPECTED, "Incorrect channel usage detected on channel: %hs", m_name.c_str());
#else

            LOG_ERROR("Incorrect channel usage detected on channel: {}", m_name);
            THROW_ERRNO(EINVAL);

#endif
        }

        gsl::span<gsl::byte> receivedSpan{};
        for (;;)
        {
            if (expectedTransactionStep == static_cast<uint32_t>(TRANSACTION_STEP::NONE))
            {
                // Adhere to the old ++ before receive behavior for non-transaction messages.
                m_received_non_transaction_messages++;
            }

            receivedSpan = ReceiveImpl(timeout);
            if (receivedSpan.empty())
            {

#ifdef WIN32
                if (errno == HCS_E_CONNECTION_TIMEOUT)
                {
                    THROW_HR_MSG(
                        HCS_E_CONNECTION_TIMEOUT,
                        "Timeout: %u, expected type: %hs, channel: %hs",
                        timeout,
                        ToString(TMessage::Type),
                        m_name.c_str());
                }
#endif

                return {nullptr, {}};
            }

            auto* header = gslhelpers::try_get_struct<MESSAGE_HEADER>(receivedSpan);
            if (header == nullptr)
            {
#ifdef WIN32
                THROW_HR_MSG(E_UNEXPECTED, "Message too small for header: %zd, channel: %hs", receivedSpan.size(), m_name.c_str());
#else
                LOG_ERROR("Message too small for header: {}, channel: {}", receivedSpan.size(), m_name);
                THROW_ERRNO(EINVAL);
#endif
            }

            if (expectedTransactionStep == static_cast<uint32_t>(TRANSACTION_STEP::NONE))
            {
                // Handle non-transaction messages with legacy logic.
                if (!m_ignore_sequence)
                {
                    if (header->TransactionStep != static_cast<unsigned int>(TRANSACTION_STEP::NONE))
                    {
#ifdef WIN32
                        THROW_HR_MSG(
                            E_UNEXPECTED,
                            "Unexpected transaction message received on non-transaction channel: %hs, message type: %hs",
                            m_name.c_str(),
                            ToString(header->MessageType));
#else
                        LOG_ERROR(
                            "Unexpected transaction message received on non-transaction channel: {}, message type: {}",
                            m_name,
                            ToString(header->MessageType));
                        THROW_ERRNO(EINVAL);
#endif
                    }
                    if (header->TransactionId != m_received_non_transaction_messages)
                    {
#ifdef WIN32
                        THROW_HR_MSG(
                            E_UNEXPECTED,
                            "Unexpected non-transaction message id: %u, expected: %u, channel: %hs",
                            header->TransactionId,
                            m_received_non_transaction_messages,
                            m_name.c_str());
#else
                        LOG_ERROR("Unexpected non-transaction message id: {}, expected: {}, channel: {}", header->TransactionId, m_received_non_transaction_messages, m_name);
                        THROW_ERRNO(EINVAL);
#endif
                    }
                }
                break;
            }

            // Handle transaction messages
            if (header->TransactionStep == static_cast<uint32_t>(TRANSACTION_STEP::NONE))
            {
                // Skip stale non-transaction messages
#ifdef WIN32
                WSL_LOG(
                    "DiscardStaleNonTransactionMessage",
                    TraceLoggingValue(m_name.c_str(), "Name"),
                    TraceLoggingValue(ToString(header->MessageType), "MessageType"),
                    TraceLoggingValue(ToString(TMessage::Type), "ExpectedMessageType"),
                    TraceLoggingValue(header->TransactionId, "StaleNonTransactionId"),
                    TraceLoggingValue(m_received_non_transaction_messages, "ExpectedNonTransactionId"));
#else
                LOG_WARNING(
                    "Discard stale non-transaction message on channel: {}. MessageType: {}, ExpectedMessageType: {}, "
                    "StaleNonTransactionId: {}, ExpectedNonTransactionId: {}",
                    m_name,
                    header->MessageType,
                    TMessage::Type,
                    header->TransactionId,
                    m_received_non_transaction_messages);
#endif
                continue;
            }

            if (expectedTransactionStep == static_cast<uint32_t>(TRANSACTION_STEP::REQUEST))
            {
                // Skip until we get the next request. No matter the transaction id.
                if (header->TransactionStep != static_cast<unsigned int>(TRANSACTION_STEP::REQUEST))
                {
#ifdef WIN32
                    WSL_LOG(
                        "DiscardOutOfOrderTransactionMessage",
                        TraceLoggingValue(m_name.c_str(), "Name"),
                        TraceLoggingValue(ToString(header->MessageType), "MessageType"),
                        TraceLoggingValue(ToString(TMessage::Type), "ExpectedMessageType"),
                        TraceLoggingValue(header->TransactionStep, "StaleTransactionStep"),
                        TraceLoggingValue(expectedTransactionStep, "ExpectedTransactionStep"));
#else
                    LOG_WARNING(
                        "Discard out of order transaction message on channel: {}. MessageType: {}, ExpectedMessageType: {}, "
                        "StaleTransactionStep: {}, ExpectedTransactionStep: {}",
                        m_name,
                        header->MessageType,
                        TMessage::Type,
                        header->TransactionStep,
                        expectedTransactionStep);
#endif
                    continue;
                }
                break;
            }

            auto diff = static_cast<int32_t>(header->TransactionId - expectedTransactionId);
            if (diff < 0)
            {
                // Skip stale transaction messages
#ifdef WIN32
                WSL_LOG(
                    "DiscardStaleTransactionMessage",
                    TraceLoggingValue(m_name.c_str(), "Name"),
                    TraceLoggingValue(ToString(header->MessageType), "MessageType"),
                    TraceLoggingValue(ToString(TMessage::Type), "ExpectedMessageType"),
                    TraceLoggingValue(header->TransactionId, "StaleTransactionId"),
                    TraceLoggingValue(expectedTransactionId, "ExpectedTransactionId"));
#else
                LOG_WARNING(
                    "Discard stale transaction message on channel: {}. MessageType: {}, ExpectedMessageType: {}, "
                    "StaleTransactionId: {}, ExpectedTransactionId: {}",
                    m_name,
                    header->MessageType,
                    TMessage::Type,
                    header->TransactionId,
                    expectedTransactionId);
#endif
                continue;
            }

            if (diff > 0)
            {
                // Message is from the future.
#ifdef WIN32
                THROW_HR_MSG(
                    E_UNEXPECTED,
                    "Unexpected transaction message id: %u, expected: %u, channel: %hs",
                    header->TransactionId,
                    expectedTransactionId,
                    m_name.c_str());
#else
                LOG_ERROR("Unexpected transaction message id: {}, expected: {}, channel: {}", header->TransactionId, expectedTransactionId, m_name);
                THROW_ERRNO(EINVAL);
#endif
            }

            if (header->TransactionStep != expectedTransactionStep)
            {
                // Broken transaction.
#ifdef WIN32
                THROW_HR_MSG(
                    E_UNEXPECTED,
                    "Unexpected transaction message step: %u, expected: %u, channel: %hs",
                    header->TransactionStep,
                    expectedTransactionStep,
                    m_name.c_str());
#else
                LOG_ERROR("Unexpected transaction message step: {}, expected: {}, channel: {}", header->TransactionStep, expectedTransactionStep, m_name);
                THROW_ERRNO(EINVAL);
#endif
            }

            break;
        }

        auto* message = gslhelpers::try_get_struct<TMessage>(receivedSpan);

        if (message == nullptr)
        {
#ifdef WIN32
            THROW_HR_MSG(
                E_UNEXPECTED,
                "Message size is too small: %zd, expected type: %hs, channel: %hs",
                receivedSpan.size(),
                ToString(TMessage::Type),
                m_name.c_str());
#else
            LOG_ERROR("MessageSize is too small: {}, expected type: {}, channel: {}", receivedSpan.size(), ToString(TMessage::Type), m_name);
            THROW_ERRNO(EINVAL);
#endif
        }

        ValidateMessageHeader(GetMessageHeader(*message), TMessage::Type);

#ifdef WIN32
        WSL_LOG(
            "ReceivedMessage",
            TraceLoggingValue(m_name.c_str(), "Name"),
            TraceLoggingValue(message->PrettyPrint().c_str(), "Content"));
#else
        if (LoggingEnabled())
        {
            LOG_INFO("ReceivedMessage on channel: {}: '{}'", m_name, message->PrettyPrint().c_str());
        }
#endif
        return {message, receivedSpan};
    }

    template <typename TMessage>
    TMessage& ReceiveMessage(
        gsl::span<gsl::byte>* responseSpan = nullptr,
        TTimeout timeout = DefaultSocketTimeout,
        uint32_t expectedTransactionStep = static_cast<uint32_t>(TRANSACTION_STEP::NONE),
        uint32_t expectedTransactionId = 0)
    {
        auto [message, span] = ReceiveMessageOrClosed<TMessage>(timeout, expectedTransactionStep, expectedTransactionId);
        if (message == nullptr)
        {
#ifdef WIN32
            THROW_HR_MSG(E_UNEXPECTED, "Expected message %hs, but socket %hs was closed", ToString(TMessage::Type), m_name.c_str());
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

    Transaction StartTransaction(TTimeout timeout = DefaultSocketTimeout)
    {
        uint32_t transactionId = m_transaction_id_seed++;
        return wsl::shared::Transaction(*this, transactionId, timeout);
    }

    Transaction ReceiveTransaction(TTimeout timeout = DefaultSocketTimeout)
    {
        // Transaction id should follow the received one on the receive end.
        return wsl::shared::Transaction(*this, 0, timeout);
    }

    template <typename TSentMessage>
    typename TSentMessage::TResponse& Transaction(gsl::span<gsl::byte> message, gsl::span<gsl::byte>* responseSpan = nullptr, TTimeout timeout = DefaultSocketTimeout)
    {
        auto transaction = StartTransaction(timeout);
        transaction.template Send<TSentMessage>(message);
        return transaction.template Receive<typename TSentMessage::TResponse>(responseSpan);
    }

    template <typename TSentMessage>
    typename TSentMessage::TResponse& Transaction(TSentMessage& message, gsl::span<gsl::byte>* responseSpan = nullptr, TTimeout timeout = DefaultSocketTimeout)
    {
        WI_ASSERT(message.Header.MessageSize == sizeof(message));

        return Transaction<TSentMessage>(gslhelpers::struct_as_writeable_bytes(message), responseSpan, timeout);
    }

    template <typename TSentMessage>
    TSentMessage::TResponse& Transaction(TTimeout timeout = DefaultSocketTimeout)
    {
        TSentMessage message{};
        message.Header.MessageSize = sizeof(message);
        message.Header.MessageType = TSentMessage::Type;
        return Transaction<TSentMessage>(message, nullptr, timeout);
    }

    void Close()
    {
        m_socket.reset();
    }

    auto Socket() const
    {
        return m_socket.get();
    }

    auto Release()
    {
        return std::move(m_socket);
    }

    bool Connected() const
    {
        return m_socket.get() >= 0;
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
    windows::common::relay::MultiHandleWait CreateIO() const
    {
        wsl::windows::common::relay::MultiHandleWait io;

        for (const auto event : m_exitEvents)
        {
            io.AddHandle(
                std::make_unique<windows::common::relay::EventHandle>(
                    event,
                    [this, event]() { THROW_HR_MSG(E_ABORT, "Exit event 0x%p signaled on channel: %hs", event, m_name.c_str()); }),
                windows::common::relay::MultiHandleWait::CancelOnCompleted | windows::common::relay::MultiHandleWait::NeedNotComplete);
        }

        return io;
    }

    static std::optional<std::chrono::milliseconds> TimeoutToMilliseconds(TTimeout timeout)
    {
        if (timeout == INFINITE)
        {
            return std::nullopt;
        }

        return std::chrono::milliseconds{timeout};
    }

    gsl::span<gsl::byte> ReceiveImpl(TTimeout timeout)
    {
        auto io = CreateIO();

        gsl::span<gsl::byte> message;

        io.AddHandle(std::make_unique<windows::common::relay::ReadSocketMessageHandle>(
            m_socket.get(), m_buffer, [&message](auto& received) { message = received; }));

        io.Run(TimeoutToMilliseconds(timeout));

        return message;
    }

#else

    gsl::span<gsl::byte> ReceiveImpl(TTimeout timeout)
    {
        return wsl::shared::socket::RecvMessage(m_socket.get(), m_buffer, timeout);
    }

#endif

    void ValidateMessageHeader(const MESSAGE_HEADER& header, LX_MESSAGE_TYPE expected) const
    {

        if (header.MessageSize < sizeof(header) || (expected != LxMiniInitMessageAny && header.MessageType != expected))
        {
#ifdef WIN32

            THROW_HR_MSG(
                E_UNEXPECTED,
                "Protocol error: Received message size: %u, type: %u, id: %u, step: %u. Expected type: %u, "
                "channel: %hs",
                header.MessageSize,
                header.MessageType,
                header.TransactionId,
                header.TransactionStep,
                expected,
                m_name.c_str());
#else

            LOG_ERROR(
                "Protocol error: Received message size: {}, type: {}, id: {}, step: {}. Expected type: {}, "
                "channel: {}",
                header.MessageSize,
                header.MessageType,
                header.TransactionId,
                header.TransactionStep,
                expected,
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

    std::vector<HANDLE> m_exitEvents;

#endif
    uint32_t m_sent_non_transaction_messages = 0;
    uint32_t m_received_non_transaction_messages = 0;
    std::atomic<uint32_t> m_transaction_id_seed = 0;
    bool m_ignore_sequence = false;
    std::string m_name{};
    std::mutex m_sendMutex;
    std::mutex m_receiveMutex;
};

inline std::optional<std::chrono::steady_clock::time_point> Transaction::ComputeDeadline(TTimeout timeout)
{
#ifdef WIN32
    if (timeout == INFINITE)
    {
        return std::nullopt;
    }

    return std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout};
#else
    if (timeout == nullptr)
    {
        return std::nullopt;
    }

    return std::chrono::steady_clock::now() + std::chrono::seconds{timeout->tv_sec} + std::chrono::microseconds{timeout->tv_usec};
#endif
}

inline TTimeout Transaction::RemainingTimeout() const
{
    if (!m_deadline.has_value())
    {
        return DefaultSocketTimeout;
    }

#ifdef WIN32

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*m_deadline - std::chrono::steady_clock::now());
    return remaining.count() > 0 ? static_cast<DWORD>(remaining.count()) : 0;

#else

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*m_deadline - std::chrono::steady_clock::now());

    m_timeoutStorage.tv_sec = static_cast<time_t>(remaining.count() / 1000);
    m_timeoutStorage.tv_usec = static_cast<suseconds_t>((remaining.count() % 1000) * 1000);
    return &m_timeoutStorage;

#endif
}

template <typename TMessage>
void Transaction::Send(gsl::span<gsl::byte> span)
{
    m_channel.SendMessage<TMessage>(span, m_step, m_id, RemainingTimeout());
    m_step++;
}

template <typename TMessage>
void Transaction::Send(TMessage& message)
{
    Send<TMessage>(gslhelpers::struct_as_writeable_bytes(message));
}

template <typename TResult>
void Transaction::SendResultMessage(TResult value)
{
    RESULT_MESSAGE<TResult> Result{};
    Result.Header.MessageSize = sizeof(Result);
    Result.Header.MessageType = RESULT_MESSAGE<TResult>::Type;
    Result.Result = value;

    Send(Result);
}

template <typename TMessage>
std::pair<TMessage*, gsl::span<gsl::byte>> Transaction::ReceiveOrClosed()
{
    auto result = m_channel.ReceiveMessageOrClosed<TMessage>(RemainingTimeout(), m_step, m_id);
    if (m_step == static_cast<uint32_t>(TRANSACTION_STEP::REQUEST) && result.first != nullptr)
    {
        // Use the request's id for the reply side transaction.
        MESSAGE_HEADER& header = m_channel.GetMessageHeader(*result.first);
        m_id = header.TransactionId;
    }
    m_step++;
    return result;
}

template <typename TMessage>
TMessage& Transaction::Receive(gsl::span<gsl::byte>* responseSpan)
{
    auto& message = m_channel.ReceiveMessage<TMessage>(responseSpan, RemainingTimeout(), m_step, m_id);
    if (m_step == static_cast<uint32_t>(TRANSACTION_STEP::REQUEST))
    {
        // Use the request's id for the reply side transaction.
        MESSAGE_HEADER& header = m_channel.GetMessageHeader(message);
        m_id = header.TransactionId;
    }
    m_step++;
    return message;
}

} // namespace wsl::shared
