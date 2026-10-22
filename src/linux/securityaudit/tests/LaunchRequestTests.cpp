// Copyright (C) Microsoft Corporation. All rights reserved.

#include <filesystem>
#include <functional>

// Compile the actual private request handler; only process creation and init logging are test substitutes.
#include "../../init/SecurityAuditLaunch.cpp"

#include <atomic>
#include <limits>
#include <stdexcept>

thread_local std::string g_threadName{"audit-launch-test"};
int g_LogFd = STDERR_FILENO;
std::optional<bool> g_EnableSocketLogging{false};

namespace {

std::atomic<unsigned int> g_peekCalls{};
std::atomic<unsigned int> g_receiveCalls{};
unsigned int g_launchCalls{};

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct SocketPair
{
    SocketPair()
    {
        int sockets[2]{};
        Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0, "Cannot create stream sockets");
        server.reset(sockets[0]);
        client.reset(sockets[1]);
        const timeval timeout{10, 0};
        Check(setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "Cannot bound test client reads");
    }

    wil::unique_fd server;
    wil::unique_fd client;
};

void WriteAll(int socket, gsl::span<const gsl::byte> buffer)
{
    while (!buffer.empty())
    {
        const auto written = TEMP_FAILURE_RETRY(send(socket, buffer.data(), buffer.size(), MSG_NOSIGNAL));
        Check(written > 0, "Cannot send stream data");
        buffer = buffer.subspan(written);
    }
}

template <typename T>
void Append(std::vector<gsl::byte>& buffer, const T& value)
{
    const auto bytes = gslhelpers::struct_as_bytes(value);
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

std::vector<gsl::byte> Request(unsigned int id = 37, const char* path = wsl::audit::c_nativeAuditCollectorPath, size_t argumentCount = 1)
{
    std::vector<gsl::byte> buffer(sizeof(CREATE_PROCESS_MESSAGE));
    CREATE_PROCESS_MESSAGE message{};
    message.Header.MessageType = LxInitCreateProcess;
    message.Header.TransactionId = id;
    message.Header.TransactionStep = static_cast<unsigned int>(TRANSACTION_STEP::REQUEST);
    message.PathIndex = buffer.size();
    const auto pathBytes = gsl::as_bytes(gsl::make_span(path, strlen(path) + 1));
    buffer.insert(buffer.end(), pathBytes.begin(), pathBytes.end());
    message.CommandLineIndex = buffer.size();
    for (size_t index = 0; index < argumentCount; ++index)
    {
        const auto length = static_cast<int32_t>(strlen(path));
        Append(buffer, length);
        const auto argument = pathBytes.first(length);
        buffer.insert(buffer.end(), argument.begin(), argument.end());
    }
    Append(buffer, int32_t{-1});
    message.Header.MessageSize = buffer.size();
    memcpy(buffer.data(), &message, sizeof(message));
    return buffer;
}

RESULT_MESSAGE<int32_t> ReadReply(int socket)
{
    RESULT_MESSAGE<int32_t> reply{};
    auto bytes = gslhelpers::struct_as_writeable_bytes(reply);
    while (!bytes.empty())
    {
        const auto received = TEMP_FAILURE_RETRY(read(socket, bytes.data(), bytes.size()));
        Check(received > 0, "Missing launch reply");
        bytes = bytes.subspan(received);
    }
    Check(
        reply.Header.MessageSize == sizeof(reply) && reply.Header.MessageType == RESULT_MESSAGE<int32_t>::Type,
        "Invalid launch reply");
    return reply;
}

void CheckReplies(int socket, unsigned int id)
{
    const auto port = ReadReply(socket);
    const auto result = ReadReply(socket);
    Check(
        port.Header.TransactionId == id && result.Header.TransactionId == id &&
            port.Header.TransactionStep == static_cast<unsigned int>(TRANSACTION_STEP::FIRST_REPLY) &&
            result.Header.TransactionStep == static_cast<unsigned int>(TRANSACTION_STEP::FIRST_REPLY) + 1 &&
            port.Result == 40123 && result.Result == 0,
        "Request/reply transaction identity or sequence changed");
}

void ExpectRejected(wil::unique_fd socket, int expectedError = 0)
{
    const auto launches = g_launchCalls;
    bool rejected = false;
    try
    {
        ServeRequest(std::move(socket));
    }
    catch (const wil::ResultException& error)
    {
        Check(expectedError == 0 || error.GetErrorCode() == expectedError, "Unexpected rejection code");
        rejected = true;
    }
    Check(rejected && g_launchCalls == launches, "Invalid request reached process creation");
}

void RejectFrame(gsl::span<const gsl::byte> frame, int expectedError = 0)
{
    SocketPair sockets;
    WriteAll(sockets.client.get(), frame);
    Check(shutdown(sockets.client.get(), SHUT_WR) == 0, "Cannot close request stream");
    ExpectRejected(std::move(sockets.server), expectedError);
}

void FragmentedRequest()
{
    SocketPair sockets;
    const auto frame = Request();
    std::exception_ptr writerError;
    std::jthread writer([&]() {
        try
        {
            // Split both the header and body while the production MSG_WAITALL/header read is in progress.
            for (size_t offset = 0; offset < frame.size();)
            {
                const auto length = std::min<size_t>(3, frame.size() - offset);
                WriteAll(sockets.client.get(), gsl::make_span(frame).subspan(offset, length));
                offset += length;
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }
        catch (...)
        {
            writerError = std::current_exception();
        }
    });
    ServeRequest(std::move(sockets.server));
    writer.join();
    if (writerError)
    {
        std::rethrow_exception(writerError);
    }
    CheckReplies(sockets.client.get(), 37);
}

void IndependentChannels()
{
    SocketPair first, second;
    WriteAll(first.client.get(), Request(19));
    WriteAll(second.client.get(), Request(987, wsl::audit::c_nativeAuditCollectorPath, 16));
    ServeRequest(std::move(second.server));
    ServeRequest(std::move(first.server));
    CheckReplies(first.client.get(), 19);
    CheckReplies(second.client.get(), 987);
}

void Bounds()
{
    for (const auto size : {c_maxLaunchMessageSize + 1, std::numeric_limits<unsigned int>::max()})
    {
        MESSAGE_HEADER header{LxInitCreateProcess, size, 9, static_cast<unsigned int>(TRANSACTION_STEP::REQUEST)};
        SocketPair sockets;
        WriteAll(sockets.client.get(), gslhelpers::struct_as_bytes(header));
        std::vector<gsl::byte> buffer;
        const auto reads = g_receiveCalls.load();
        Check(
            wsl::shared::socket::RecvMessage(sockets.server.get(), buffer, nullptr, c_maxLaunchMessageSize).empty(),
            "Oversized frame accepted");
        Check(
            buffer.size() == sizeof(MESSAGE_HEADER) && buffer.capacity() < c_maxLaunchMessageSize && g_receiveCalls == reads + 1,
            "Oversized body was allocated or read before checking the limit");

        SocketPair production;
        WriteAll(production.client.get(), gslhelpers::struct_as_bytes(header));
        const auto productionReads = g_receiveCalls.load();
        ExpectRejected(std::move(production.server));
        Check(g_receiveCalls == productionReads + 1, "Production launch read an oversized body");
    }

    auto frame = Request();
    frame.resize(c_maxLaunchMessageSize);
    reinterpret_cast<MESSAGE_HEADER*>(frame.data())->MessageSize = frame.size();
    SocketPair boundary;
    WriteAll(boundary.client.get(), frame);
    ServeRequest(std::move(boundary.server));
    CheckReplies(boundary.client.get(), 37);

    // Existing channels retain their larger default; moving a bounded channel must retain its limit.
    frame.resize(c_maxLaunchMessageSize + 1);
    reinterpret_cast<MESSAGE_HEADER*>(frame.data())->MessageSize = frame.size();
    SocketPair defaults;
    WriteAll(defaults.client.get(), frame);
    wsl::shared::SocketChannel defaultChannel{std::move(defaults.server), "DefaultLimit"};
    auto transaction = defaultChannel.ReceiveTransaction();
    gsl::span<gsl::byte> received;
    transaction.Receive<CREATE_PROCESS_MESSAGE>(&received);
    Check(received.size() == frame.size(), "Default channel limit was reduced");

    SocketPair moved;
    WriteAll(moved.client.get(), gsl::make_span(frame).first(sizeof(MESSAGE_HEADER)));
    wsl::shared::SocketChannel bounded{std::move(moved.server), "Bounded", c_maxLaunchMessageSize};
    wsl::shared::SocketChannel movedChannel{std::move(bounded)};
    wsl::shared::SocketChannel assignedChannel;
    assignedChannel = std::move(movedChannel);
    auto movedTransaction = assignedChannel.ReceiveTransaction();
    Check(movedTransaction.ReceiveOrClosed<CREATE_PROCESS_MESSAGE>().first == nullptr, "Moved channel lost its limit");

    SocketPair globalLimit;
    MESSAGE_HEADER header{LxInitCreateProcess, wsl::shared::socket::c_maxMessageSize + 1, 9, 1};
    WriteAll(globalLimit.client.get(), gslhelpers::struct_as_bytes(header));
    std::vector<gsl::byte> buffer;
    Check(
        wsl::shared::socket::RecvMessage(globalLimit.server.get(), buffer, nullptr, std::numeric_limits<size_t>::max()).empty() &&
            buffer.size() == sizeof(MESSAGE_HEADER),
        "Optional limit relaxed the existing 16 MiB ceiling");
}

void InvalidRequests()
{
    RejectFrame(Request(1, "/bin/sh"), EPERM);
    RejectFrame(Request(1, wsl::audit::c_nativeAuditCollectorPath, 0), EINVAL);
    RejectFrame(Request(1, wsl::audit::c_nativeAuditCollectorPath, 17), EINVAL);
    auto frame = Request();
    reinterpret_cast<MESSAGE_HEADER*>(frame.data())->MessageType = LxMiniInitMessageAny;
    RejectFrame(frame, EINVAL);
    frame = Request();
    auto* message = reinterpret_cast<CREATE_PROCESS_MESSAGE*>(frame.data());
    message->PathIndex = frame.size();
    RejectFrame(frame, EINVAL);
    frame = Request();
    message = reinterpret_cast<CREATE_PROCESS_MESSAGE*>(frame.data());
    message->CommandLineIndex = frame.size();
    RejectFrame(frame, EINVAL);

    frame = Request();
    for (const auto length : {size_t{0}, sizeof(MESSAGE_HEADER) - 1, sizeof(MESSAGE_HEADER), frame.size() - 1})
    {
        RejectFrame(gsl::make_span(frame).first(length));
    }
    for (const auto size : {sizeof(MESSAGE_HEADER) - 1, sizeof(MESSAGE_HEADER), sizeof(CREATE_PROCESS_MESSAGE) - 1})
    {
        reinterpret_cast<MESSAGE_HEADER*>(frame.data())->MessageSize = size;
        RejectFrame(gsl::make_span(frame).first(std::max(size, sizeof(MESSAGE_HEADER))));
    }
}

void ReceiveTimeouts()
{
    const auto frame = Request();
    for (const auto length : {size_t{0}, sizeof(MESSAGE_HEADER) - 1, sizeof(MESSAGE_HEADER), frame.size() - 1})
    {
        SocketPair sockets;
        wil::unique_fd timeoutCheck{dup(sockets.server.get())};
        Check(static_cast<bool>(timeoutCheck), "Cannot retain socket timeout settings");
        WriteAll(sockets.client.get(), gsl::make_span(frame).first(length));
        const auto start = std::chrono::steady_clock::now();
        ExpectRejected(std::move(sockets.server));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Check(
            elapsed >= std::chrono::seconds{4} && elapsed < std::chrono::seconds{12},
            "Stalled header/body did not observe the production 5 second socket receive timeout");
        for (const auto option : {SO_RCVTIMEO, SO_SNDTIMEO})
        {
            timeval timeout{};
            socklen_t size = sizeof(timeout);
            Check(
                getsockopt(timeoutCheck.get(), SOL_SOCKET, option, &timeout, &size) == 0 && size == sizeof(timeout) &&
                    timeout.tv_sec == 5 && timeout.tv_usec == 0,
                "Production socket receive/send timeout changed");
        }
    }
}

} // namespace

extern "C" ssize_t __real_recv(int socket, void* buffer, size_t length, int flags);
extern "C" ssize_t __wrap_recv(int socket, void* buffer, size_t length, int flags)
{
    ++g_receiveCalls;
    if ((flags & MSG_PEEK) != 0)
    {
        ++g_peekCalls;
        errno = EOPNOTSUPP;
        return -1;
    }
    return __real_recv(socket, buffer, length, flags);
}

ssize_t UtilWriteBuffer(int socket, const void* buffer, size_t length)
{
    WriteAll(socket, gsl::make_span(static_cast<const gsl::byte*>(buffer), length));
    return length;
}

std::string UtilReadFileContent(std::string_view)
{
    throw std::runtime_error("Unexpected init logging setup");
}

int ProcessCreateProcessMessage(wsl::shared::Transaction& transaction, gsl::span<gsl::byte>, const std::optional<std::string>& cgroup, int namespaceFd, bool hostOnly)
{
    Check(!cgroup.has_value() && namespaceFd == -1 && hostOnly, "Launch process isolation arguments changed");
    ++g_launchCalls;
    transaction.SendResultMessage<int32_t>(40123);
    transaction.SendResultMessage<int32_t>(0);
    return 0;
}

int main()
{
    try
    {
        FragmentedRequest();
        IndependentChannels();
        Bounds();
        InvalidRequests();
        ReceiveTimeouts();
        Check(g_receiveCalls > 0 && g_peekCalls == 0, "Production decoder used unsupported MSG_PEEK");
        std::puts(
            "PASS: production launch decoder, fragmentation, allocation bounds, validation, EOF, timeouts, transaction IDs");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
