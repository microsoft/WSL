// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "p9defs.h"

namespace Plan9ProtocolTests {
namespace {

    using Plan9MessageType = ::p9fs::MessageType;

    constexpr std::array<uint8_t, 21> c_versionRequest = {
        0x15, 0x00, 0x00, 0x00, static_cast<uint8_t>(Plan9MessageType::Tversion),
        0xff, 0xff, 0x00, 0x00, 0x01,
        0x00, 0x08, 0x00, '9',  'P',
        '2',  '0',  '0',  '0',  '.',
        'L'};
    constexpr std::array<uint8_t, 21> c_versionResponse = {
        0x15, 0x00, 0x00, 0x00, static_cast<uint8_t>(Plan9MessageType::Rversion),
        0xff, 0xff, 0x00, 0x00, 0x01,
        0x00, 0x08, 0x00, '9',  'P',
        '2',  '0',  '0',  '0',  '.',
        'L'};
    constexpr std::array<uint8_t, 11> c_invalidArgumentResponse = {
        0x0b, 0x00, 0x00, 0x00, static_cast<uint8_t>(Plan9MessageType::Rlerror), 0xff, 0xff, EINVAL, 0x00, 0x00, 0x00};
    constexpr uint16_t c_plan9Port = 1234;

    void AppendU8(std::vector<uint8_t>& buffer, uint8_t value)
    {
        buffer.push_back(value);
    }

    void AppendU16(std::vector<uint8_t>& buffer, uint16_t value)
    {
        buffer.push_back(static_cast<uint8_t>(value));
        buffer.push_back(static_cast<uint8_t>(value >> 8));
    }

    void AppendU32(std::vector<uint8_t>& buffer, uint32_t value)
    {
        for (size_t index = 0; index < sizeof(value); ++index)
        {
            buffer.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    }

    void AppendU64(std::vector<uint8_t>& buffer, uint64_t value)
    {
        for (size_t index = 0; index < sizeof(value); ++index)
        {
            buffer.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    }

    void AppendString(std::vector<uint8_t>& buffer, std::string_view value)
    {
        AppendU16(buffer, gsl::narrow<uint16_t>(value.size()));
        buffer.insert(buffer.end(), value.begin(), value.end());
    }

    std::vector<uint8_t> MakePlan9Message(Plan9MessageType type, std::vector<uint8_t> payload)
    {
        std::vector<uint8_t> message;
        message.reserve(sizeof(uint32_t) + sizeof(type) + sizeof(uint16_t) + payload.size());
        AppendU32(message, gsl::narrow<uint32_t>(sizeof(uint32_t) + sizeof(type) + sizeof(uint16_t) + payload.size()));
        AppendU8(message, static_cast<uint8_t>(type));
        AppendU16(message, UINT16_MAX);
        message.insert(message.end(), payload.begin(), payload.end());
        return message;
    }

    struct Plan9Server
    {
        wil::unique_handle process;
        wil::unique_socket client;
        wil::unique_hfile stdinPipe;

        Plan9Server() = default;
        Plan9Server(Plan9Server&&) = default;
        Plan9Server& operator=(Plan9Server&&) = default;
        Plan9Server(const Plan9Server&) = delete;
        Plan9Server& operator=(const Plan9Server&) = delete;

        ~Plan9Server()
        {
            stdinPipe.reset();
            if (process)
            {
                VERIFY_ARE_EQUAL(wsl::windows::common::SubProcess::GetExitCode(process.get(), 30000), 0);
            }
        }
    };

    Plan9Server ConnectToServer()
    {
        auto [stdinRead, stdinWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdinRead.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(writePipe.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

        auto commandLine = LxssGenerateWslCommandLine(std::format(L"/plan9 --bind {}", c_plan9Port).c_str());
        Plan9Server server;
        server.process = LxsstuStartProcess(commandLine.data(), stdinRead.get(), writePipe.get());
        server.stdinPipe = std::move(stdinWrite);
        stdinRead.reset();
        writePipe.reset();

        PartialHandleRead output{readPipe.get()};
        output.Expect(std::format("bound port {}\n", c_plan9Port));

        SOCKADDR_IN address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(c_plan9Port);

        server.client.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        THROW_LAST_ERROR_IF(!server.client);
        THROW_LAST_ERROR_IF(connect(server.client.get(), reinterpret_cast<SOCKADDR*>(&address), sizeof(address)) == SOCKET_ERROR);
        return server;
    }

    void SendAll(SOCKET socket, std::span<const uint8_t> buffer)
    {
        size_t offset = 0;
        while (offset < buffer.size())
        {
            const auto sent =
                send(socket, reinterpret_cast<const char*>(buffer.data() + offset), gsl::narrow<int>(buffer.size() - offset), 0);
            THROW_LAST_ERROR_IF(sent == SOCKET_ERROR);
            THROW_HR_IF(E_UNEXPECTED, sent == 0);
            offset += sent;
        }
    }

    std::vector<uint8_t> ReceivePlan9Message(SOCKET socket)
    {
        const auto receiveExact = [socket](std::span<uint8_t> buffer) {
            size_t offset = 0;
            while (offset < buffer.size())
            {
                const auto received =
                    recv(socket, reinterpret_cast<char*>(buffer.data() + offset), gsl::narrow<int>(buffer.size() - offset), 0);
                THROW_LAST_ERROR_IF(received == SOCKET_ERROR);
                THROW_HR_IF(E_UNEXPECTED, received == 0);
                offset += received;
            }
        };

        std::array<uint8_t, sizeof(uint32_t)> header{};
        receiveExact(header);

        const uint32_t messageSize = header[0] | (static_cast<uint32_t>(header[1]) << 8) |
                                     (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
        THROW_HR_IF(E_UNEXPECTED, messageSize < header.size() || messageSize > 256 * 1024);

        std::vector<uint8_t> message(messageSize);
        std::copy(header.begin(), header.end(), message.begin());
        receiveExact(std::span{message}.subspan(header.size()));
        return message;
    }

    void SendMessageAndExpectResponse(SOCKET socket, const std::vector<uint8_t>& request, Plan9MessageType responseType)
    {
        SendAll(socket, request);
        const auto response = ReceivePlan9Message(socket);
        VERIFY_IS_GREATER_THAN_OR_EQUAL(response.size(), static_cast<size_t>(5));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(responseType), response[4]);
    }

} // namespace

class Plan9ProtocolTests
{
    WSL_TEST_CLASS(Plan9ProtocolTests)

    std::optional<WslConfigChange> m_config;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        if (LxsstuVmMode())
        {
            m_config.emplace(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        m_config.reset();
        VERIFY_NO_THROW(LxsstuUninitialize(FALSE));
        return true;
    }

    // Smoke test
    WSL2_TEST_METHOD(QueryVersion)
    {
        m_config->Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ln -sf /init /plan9"), 0u);

        auto server = ConnectToServer();

        SendAll(server.client.get(), c_versionRequest);

        const auto response = ReceivePlan9Message(server.client.get());
        VERIFY_IS_TRUE(std::ranges::equal(c_versionResponse, response));
    }

    // Send a truncated tread and validate that the server return returns EINVAL.
    WSL2_TEST_METHOD(ReadRejectsTruncatedRequest)
    {
        m_config->Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ln -sf /init /plan9"), 0u);

        auto server = ConnectToServer();

        SendAll(server.client.get(), MakePlan9Message(Plan9MessageType::Tread, {}));

        const auto response = ReceivePlan9Message(server.client.get());
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, response));
    }

    // Validate that the server rejects client ids that extend passed the end of the message.
    WSL2_TEST_METHOD(GetLockRejectsOversizedClientId)
    {
        m_config->Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ln -sf /init /plan9"), 0u);

        auto server = ConnectToServer();

        std::vector<uint8_t> payload;
        AppendU32(payload, 8192);
        AppendString(payload, "9P2000.L");
        SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Tversion, std::move(payload)), Plan9MessageType::Rversion);

        payload.clear();
        AppendU32(payload, 1);
        AppendU32(payload, UINT32_MAX);
        AppendString(payload, "");
        AppendString(payload, "");
        AppendU32(payload, 0);
        SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Tattach, std::move(payload)), Plan9MessageType::Rattach);

        payload.clear();
        AppendU32(payload, 1);
        AppendU32(payload, 0);
        SendMessageAndExpectResponse(server.client.get(), MakePlan9Message(Plan9MessageType::Tlopen, std::move(payload)), Plan9MessageType::Rlopen);

        payload.clear();
        AppendU32(payload, 1);
        AppendU8(payload, 0);
        AppendU64(payload, 0);
        AppendU64(payload, 0);
        AppendU32(payload, 0);
        AppendString(payload, std::string(4096, 'A'));
        SendAll(server.client.get(), MakePlan9Message(Plan9MessageType::Tgetlock, std::move(payload)));

        const auto response = ReceivePlan9Message(server.client.get());
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, response));

        payload.clear();
        AppendU8(payload, 0);
        AppendU64(payload, 0);
        AppendU64(payload, 0);
        AppendU32(payload, 0);
        AppendU16(payload, 4096);
        SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Rgetlock, std::move(payload)), Plan9MessageType::Rlerror);
    }
};
} // namespace Plan9ProtocolTests
