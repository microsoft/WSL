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
        wil::unique_hfile outputPipe;
        std::unique_ptr<PartialHandleRead> output;

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

            if (output)
            {
                output->ExpectClosed();
                LogInfo("Plan9 logs: %hs", output->GetData().c_str());
            }
        }
    };

    Plan9Server ConnectToServer()
    {
        auto [stdinRead, stdinWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        auto [logRead, logWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdinRead.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdoutWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(logWrite.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

        // TRACE_LEVEL_VERBOSE == 5
        auto commandLine = LxssGenerateWslCommandLine(
            std::format(L"-u root /plan9 --bind {} --log-file /dev/stderr --log-level {}", c_plan9Port, 5).c_str());

        Plan9Server server;
        server.process = LxsstuStartProcess(commandLine.data(), stdinRead.get(), stdoutWrite.get(), logWrite.get());
        server.stdinPipe = std::move(stdinWrite);
        server.outputPipe = std::move(logRead);
        server.output = std::make_unique<PartialHandleRead>(server.outputPipe.get());
        stdinRead.reset();
        stdoutWrite.reset();
        logWrite.reset();

        // Wait for the port to be bound before connecting.
        PartialHandleRead stdoutReader{stdoutRead.get()};
        stdoutReader.Expect(std::format("bound port {}\n", c_plan9Port));

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

    std::vector<uint8_t> SendMessageAndExpectResponse(SOCKET socket, std::span<const uint8_t> request, Plan9MessageType responseType)
    {
        SendAll(socket, request);
        auto response = ReceivePlan9Message(socket);
        VERIFY_IS_GREATER_THAN_OR_EQUAL(response.size(), static_cast<size_t>(5));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(responseType), response[4]);
        return response;
    }

    Plan9Server ConnectToOpenedServer()
    {
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

        return server;
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
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root ln -sf /init /plan9"), 0u);
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_config)
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root rm -f /plan9"), 0u);
        }

        m_config.reset();
        VERIFY_NO_THROW(LxsstuUninitialize(FALSE));
        return true;
    }

    // Smoke test
    WSL2_TEST_METHOD(QueryVersion)
    {
        auto server = ConnectToServer();

        const auto response = SendMessageAndExpectResponse(server.client.get(), c_versionRequest, Plan9MessageType::Rversion);
        VERIFY_IS_TRUE(std::ranges::equal(c_versionResponse, response));
    }

    WSL2_TEST_METHOD(XattrCommitsUseAttachedCredentials)
    {
        ULONG uid{};
        ULONG gid{};
        CreateUser(L"plan9testuser", &uid, &gid);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root rm -f /tmp/plan9-xattr-test"), 0u);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root userdel plan9testuser"), 0u);
        });

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root install -m 644 /dev/null /tmp/plan9-xattr-test"), 0u);

        auto server = ConnectToServer();
        const auto socket = server.client.get();
        SendMessageAndExpectResponse(socket, c_versionRequest, Plan9MessageType::Rversion);

        constexpr uint32_t c_rootFid = 1;
        constexpr uint32_t c_userFid = 2;
        constexpr uint32_t c_xattrFid = 3;
        constexpr uint32_t c_attachFid = 4;
        constexpr uint32_t c_xattrCreate = 1;
        constexpr uint32_t c_xattrReplace = 2;
        constexpr std::string_view c_attribute = "user.plan9-test";

        const auto attachFile = [&](uint32_t fid, uint32_t attachUid) {
            std::vector<uint8_t> payload;
            AppendU32(payload, c_attachFid);
            AppendU32(payload, UINT32_MAX);
            AppendString(payload, "");
            AppendString(payload, "");
            AppendU32(payload, attachUid);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Tattach, std::move(payload)), Plan9MessageType::Rattach);

            payload.clear();
            AppendU32(payload, c_attachFid);
            AppendU32(payload, fid);
            AppendU16(payload, 2);
            AppendString(payload, "tmp");
            AppendString(payload, "plan9-xattr-test");
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Twalk, std::move(payload)), Plan9MessageType::Rwalk);

            payload.clear();
            AppendU32(payload, c_attachFid);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Tclunk, std::move(payload)), Plan9MessageType::Rclunk);
        };

        attachFile(c_rootFid, 0);
        attachFile(c_userFid, uid);

        const auto mutateAttribute = [&](uint32_t fid, std::string_view value, uint32_t flags, bool allowed) {
            std::vector<uint8_t> payload;
            AppendU32(payload, fid);
            AppendU32(payload, c_xattrFid);
            AppendU16(payload, 0);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Twalk, std::move(payload)), Plan9MessageType::Rwalk);

            payload.clear();
            AppendU32(payload, c_xattrFid);
            AppendString(payload, c_attribute);
            AppendU64(payload, value.size());
            AppendU32(payload, flags);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Txattrcreate, std::move(payload)), Plan9MessageType::Rxattrcreate);

            if (!value.empty())
            {
                payload.clear();
                AppendU32(payload, c_xattrFid);
                AppendU64(payload, 0);
                AppendU32(payload, gsl::narrow<uint32_t>(value.size()));
                payload.insert(payload.end(), value.begin(), value.end());
                SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Twrite, std::move(payload)), Plan9MessageType::Rwrite);
            }

            payload.clear();
            AppendU32(payload, c_xattrFid);
            const auto responseType = allowed ? Plan9MessageType::Rclunk : Plan9MessageType::Rlerror;
            const auto response =
                SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Tclunk, std::move(payload)), responseType);

            std::vector<uint8_t> expectedPayload;
            if (!allowed)
            {
                AppendU32(expectedPayload, EACCES);
            }

            const auto expected = MakePlan9Message(responseType, std::move(expectedPayload));
            VERIFY_IS_TRUE(std::ranges::equal(expected, response));
        };

        // Authorization is enforced at clunk, not xattrcreate.
        mutateAttribute(c_userFid, "denied", c_xattrCreate, false);
        mutateAttribute(c_rootFid, "original", c_xattrCreate, true);
        mutateAttribute(c_userFid, "denied", c_xattrReplace, false);
        mutateAttribute(c_userFid, "", 0, false);

        // Validate that the attribute value is still 'original'.
        {
            std::vector<uint8_t> payload;
            AppendU32(payload, c_rootFid);
            AppendU32(payload, c_xattrFid);
            AppendString(payload, c_attribute);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Txattrwalk, std::move(payload)), Plan9MessageType::Rxattrwalk);

            payload.clear();

            AppendU32(payload, c_xattrFid);
            AppendU64(payload, 0);
            AppendU32(payload, 8);
            const auto response =
                SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Tread, std::move(payload)), Plan9MessageType::Rread);

            payload.clear();

            AppendU32(payload, 8);
            constexpr std::string_view c_original = "original";
            payload.insert(payload.end(), c_original.begin(), c_original.end());
            VERIFY_IS_TRUE(std::ranges::equal(MakePlan9Message(Plan9MessageType::Rread, std::move(payload)), response));

            payload.clear();
            AppendU32(payload, c_xattrFid);
            SendMessageAndExpectResponse(socket, MakePlan9Message(Plan9MessageType::Tclunk, std::move(payload)), Plan9MessageType::Rclunk);
            mutateAttribute(c_rootFid, "", 0, true);
        }

        // Validate that the xattr can be modified when the user has write access to it.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"-u root chown {}:{} /tmp/plan9-xattr-test", uid, gid)), 0u);
        mutateAttribute(c_userFid, "created", c_xattrCreate, true);
        mutateAttribute(c_userFid, "replaced", c_xattrReplace, true);
        mutateAttribute(c_userFid, "", 0, true);
        mutateAttribute(c_rootFid, "restored", c_xattrCreate, true);
        mutateAttribute(c_rootFid, "", 0, true);
    }

    // Send a truncated tread and validate that the server returns EINVAL.
    WSL2_TEST_METHOD(ReadRejectsTruncatedRequest)
    {
        auto server = ConnectToServer();

        const auto response =
            SendMessageAndExpectResponse(server.client.get(), MakePlan9Message(Plan9MessageType::Tread, {}), Plan9MessageType::Rlerror);
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, response));
    }

    // Validate that logging and processing a string that extends past the end of the message returns EINVAL.
    WSL2_TEST_METHOD(VersionRejectsTruncatedString)
    {
        auto server = ConnectToServer();

        std::vector<uint8_t> payload;
        AppendU32(payload, 8192);
        AppendU16(payload, 1);

        auto response = SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Tversion, std::move(payload)), Plan9MessageType::Rlerror);
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, response));

        response = SendMessageAndExpectResponse(server.client.get(), c_versionRequest, Plan9MessageType::Rversion);
        VERIFY_IS_TRUE(std::ranges::equal(c_versionResponse, response));
    }

    // Validate that the lock handlers reject client ids that extend past the end of the message.
    WSL2_TEST_METHOD(LockRequestsRejectTruncatedClientId)
    {
        auto server = ConnectToOpenedServer();

        std::vector<uint8_t> payload;
        AppendU32(payload, 1);
        AppendU8(payload, 0);
        AppendU64(payload, 0);
        AppendU64(payload, 0);
        AppendU32(payload, 0);
        AppendU16(payload, 1);

        const auto response = SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Tgetlock, std::move(payload)), Plan9MessageType::Rlerror);
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, response));

        payload.clear();
        AppendU32(payload, 1);
        AppendU8(payload, 0);
        AppendU32(payload, 0);
        AppendU64(payload, 0);
        AppendU64(payload, 0);
        AppendU32(payload, 0);
        AppendU16(payload, 1);

        const auto lockResponse = SendMessageAndExpectResponse(
            server.client.get(), MakePlan9Message(Plan9MessageType::Tlock, std::move(payload)), Plan9MessageType::Rlerror);
        VERIFY_IS_TRUE(std::ranges::equal(c_invalidArgumentResponse, lockResponse));
    }
};
} // namespace Plan9ProtocolTests
