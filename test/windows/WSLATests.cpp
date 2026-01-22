/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLATests.cpp

Abstract:

    This file contains test cases for the WSLA API.

--*/

#include "precomp.h"
#include "Common.h"
#include "WSLAApi.h"
#include "wslaservice.h"
#include "WSLAProcessLauncher.h"
#include "WSLAContainerLauncher.h"
#include "WslCoreFilesystem.h"

using namespace wsl::windows::common::registry;
using wsl::windows::common::ProcessFlags;
using wsl::windows::common::RunningWSLAContainer;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAContainerLauncher;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

DEFINE_ENUM_FLAG_OPERATORS(WSLAFeatureFlags);

static std::filesystem::path storagePath;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

class WSLATests
{
    WSL_TEST_CLASS(WSLATests)
    wil::unique_couninitialize_call coinit = wil::CoInitializeEx();
    WSADATA Data;
    std::filesystem::path testVhd;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

        auto distroKey = OpenDistributionKey(LXSS_DISTRO_NAME_TEST_L);

        auto vhdPath = wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, L"BasePath");
        testVhd = std::filesystem::path{vhdPath} / "ext4.vhdx";
        storagePath = std::filesystem::current_path() / "test-storage";

        auto session = CreateSession();

        wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
        VERIFY_SUCCEEDED(session->ListImages(&images, images.size_address<ULONG>()));

        auto hasImage = [&](const std::string& imageName) {
            return std::ranges::any_of(
                images.get(), images.get() + images.size(), [&](const auto& e) { return e.Image == imageName; });
        };

        if (!hasImage("debian:latest"))
        {
            VERIFY_SUCCEEDED(session->PullImage("debian:latest", nullptr, nullptr, nullptr));
        }

        if (!hasImage("python:3.12-alpine"))
        {
            VERIFY_SUCCEEDED(session->PullImage("python:3.12-alpine", nullptr, nullptr, nullptr));
        }

        // Hacky way to delete all containers.
        // TODO: Replace with the --rm flag once available.
        ExpectCommandResult(session.get(), {"/usr/bin/docker", "container", "prune", "-f"}, 0);

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        // Keep the VHD when running in -f mode, to speed up subsequent test runs.
        if (!g_fastTestRun && !storagePath.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(storagePath, error);
            if (error)
            {
                LogError("Failed to cleanup storage path %ws: %hs", storagePath.c_str(), error.message().c_str());
            }
        }

        return true;
    }

    static WSLA_SESSION_SETTINGS GetDefaultSessionSettings()
    {
        WSLA_SESSION_SETTINGS settings{};
        settings.DisplayName = L"wsla-test";
        settings.CpuCount = 4;
        settings.MemoryMb = 2024;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = storagePath.c_str();
        settings.MaximumStorageSizeMb = 1000; // 1GB.
        settings.NetworkingMode = WSLANetworkingModeNAT;

        return settings;
    }

    wil::com_ptr<IWSLASession> CreateSession(const WSLA_SESSION_SETTINGS& sessionSettings = GetDefaultSessionSettings(), WSLASessionFlags Flags = WSLASessionFlagsNone)
    {
        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        wil::com_ptr<IWSLASession> session;

        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        return session;
    }

    TEST_METHOD(GetVersion)
    {
        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        WSLA_VERSION version{};

        VERIFY_SUCCEEDED(sessionManager->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    RunningWSLAProcess::ProcessResult RunCommand(IWSLASession* session, const std::vector<std::string>& command, int timeout = 600000)
    {
        WSLAProcessLauncher process(command[0], command);

        return process.Launch(*session).WaitAndCaptureOutput();
    }

    RunningWSLAProcess::ProcessResult ExpectCommandResult(
        IWSLASession* session, const std::vector<std::string>& command, int expectResult, bool expectSignal = false, int timeout = 600000)
    {
        auto result = RunCommand(session, command, timeout);

        if (result.Code != expectResult)
        {
            auto cmd = wsl::shared::string::Join(command, ' ');
            LogError(
                "Command: %hs didn't return expected code (%i). ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                cmd.c_str(),
                expectResult,
                result.Code,
                result.Output[1].c_str(),
                result.Output[2].c_str());
        }

        return result;
    }

    void ValidateProcessOutput(RunningWSLAProcess& process, const std::map<int, std::string>& expectedOutput, int expectedResult = 0)
    {
        auto result = process.WaitAndCaptureOutput();

        if (result.Code != expectedResult)
        {
            LogError(
                "Comman didn't return expected code (%i). ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                expectedResult,
                result.Code,
                result.Output[1].c_str(),
                result.Output[2].c_str());

            return;
        }

        for (const auto& [fd, expected] : expectedOutput)
        {
            auto it = result.Output.find(fd);
            if (it == result.Output.end())
            {
                LogError("Expected output on fd %i, but none found.", fd);
                return;
            }

            if (it->second != expected)
            {
                LogError("Unexpected output on fd %i. Expected: '%hs', Actual: '%hs'", fd, expected.c_str(), it->second.c_str());
            }
        }
    }

    void ExpectMount(IWSLASession* session, const std::string& target, const std::optional<std::string>& options)
    {
        auto cmd = std::format("set -o pipefail ; findmnt '{}' | tail  -n 1", target);
        auto result = ExpectCommandResult(session, {"/bin/sh", "-c", cmd}, options.has_value() ? 0 : 1);

        const auto& output = result.Output[1];
        const auto& error = result.Output[2];

        if (result.Code != (options.has_value() ? 0 : 1))
        {
            LogError("%hs failed. code=%i, output: %hs, error: %hs", cmd.c_str(), result.Code, output.c_str(), error.c_str());
            VERIFY_FAIL();
        }

        if (options.has_value() && !PathMatchSpecA(output.c_str(), options->c_str()))
        {
            std::wstring message = std::format(L"Output: '{}' didn't match pattern: '{}'", output, options.value());
            VERIFY_FAIL(message.c_str());
        }
    }

    TEST_METHOD(ListSessionsReturnsSessionWithDisplayName)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.StoragePath = nullptr;
        settings.DisplayName = L"wsla-test-list";

        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        wil::com_ptr<IWSLASession> session = CreateSession(settings);

        // Act: list sessions
        {
            wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            // Assert
            VERIFY_ARE_EQUAL(sessions.size(), 1u);
            const auto& info = sessions[0];

            // SessionId is implementation detail (starts at 1), so we only assert DisplayName here.
            VERIFY_ARE_EQUAL(std::wstring(info.DisplayName), std::wstring(L"wsla-test-list"));
        }

        // List multiple sessions.
        {
            settings.DisplayName = L"wsla-test-list-2";
            auto session2 = CreateSession(settings);

            wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            VERIFY_ARE_EQUAL(sessions.size(), 2);

            std::vector<std::wstring> displayNames;
            for (const auto& e : sessions)
            {
                displayNames.push_back(e.DisplayName);
            }

            std::ranges::sort(displayNames);

            VERIFY_ARE_EQUAL(displayNames[0], L"wsla-test-list");
            VERIFY_ARE_EQUAL(displayNames[1], L"wsla-test-list-2");
        }
    }

    TEST_METHOD(OpenSessionByNameFindsExistingSession)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.StoragePath = nullptr;
        settings.DisplayName = L"wsla-open-by-name-test";

        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        wil::com_ptr<IWSLASession> created = CreateSession(settings);

        // Act: open by the same display name
        wil::com_ptr<IWSLASession> opened;
        VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(L"wsla-open-by-name-test", &opened));
        VERIFY_IS_NOT_NULL(opened.get());

        // And verify we get ERROR_NOT_FOUND for a nonexistent name
        wil::com_ptr<IWSLASession> notFound;
        auto hr = sessionManager->OpenSessionByName(L"this-name-does-not-exist", &notFound);
        VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    void ExpectImagePresent(IWSLASession& Session, const char* Image, bool Present = true)
    {
        wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
        THROW_IF_FAILED(Session.ListImages(images.addressof(), images.size_address<ULONG>()));

        std::vector<std::string> tags;
        for (const auto& e : images)
        {
            tags.push_back(e.Image);
        }

        if (Present)
        {
            VERIFY_IS_TRUE(std::ranges::find(tags, Image) != tags.end());
        }
        else
        {
            VERIFY_IS_TRUE(std::ranges::find(tags, Image) == tags.end());
        }
    }

    TEST_METHOD(PullImage)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.DisplayName = L"wsla-pull-image-test";
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        {
            VERIFY_SUCCEEDED(session->PullImage("hello-world:linux", nullptr, nullptr, nullptr));

            // Verify that the image is in the list of images.
            ExpectImagePresent(*session, "hello-world:linux");
            WSLAContainerLauncher launcher("hello-world:linux", "wsla-pull-image-container");

            auto container = launcher.Launch(*session);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        {
            std::string expectedError =
                "pull access denied for does-not, repository does not exist or may require 'docker login': denied: requested "
                "access to the resource is denied";

            WSLAErrorDetails error;
            VERIFY_ARE_EQUAL(session->PullImage("does-not:exist", nullptr, nullptr, &error.Error), WSLA_E_IMAGE_NOT_FOUND);
            VERIFY_ARE_EQUAL(expectedError, error.Error.UserErrorMessage);
        }
    }

    // TODO: Test that invalid tars are correctly handled.
    TEST_METHOD(LoadImage)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.DisplayName = L"wsla-load-image-test";

        auto session = CreateSession(settings);

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldSaved.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(session->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*session, "hello-world:latest");
        WSLAContainerLauncher launcher("hello-world:latest", "wsla-load-image-container");

        auto container = launcher.Launch(*session);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
    }

    // TODO: Test that invalid tars are correctly handled.
    TEST_METHOD(ImportImage)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.DisplayName = L"wsla-import-image-test";

        auto session = CreateSession(settings);

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(session->ImportImage(HandleToULong(imageTarFileHandle.get()), "my-hello-world:test", nullptr, fileSize.QuadPart));

        ExpectImagePresent(*session, "my-hello-world:test");

        // Validate that containers can be started from the imported image.
        WSLAContainerLauncher launcher("my-hello-world:test", "wsla-import-image-container", "/hello");

        auto container = launcher.Launch(*session);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
    }
    TEST_METHOD(DeleteImage)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.DisplayName = L"wsla-delete-image-test";
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        // Prepare alpine image to delete.
        VERIFY_SUCCEEDED(session->PullImage("alpine:latest", nullptr, nullptr, nullptr));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*session, "alpine:latest");

        // Launch a container to ensure that image deletion fails when in use.
        WSLAContainerLauncher launcher(
            "alpine:latest", "test-delete-container-in-use", "sleep", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

        auto container = launcher.Launch(*session);

        // Verify that the container is in running state.
        VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

        // Test delete failed if image in use.
        WSLA_DELETE_IMAGE_OPTIONS options{};
        options.Image = "alpine:latest";
        options.Force = FALSE;
        wil::unique_cotaskmem_array_ptr<WSLA_DELETED_IMAGE_INFORMATION> deletedImages;

        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            session->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>(), nullptr));

        // Force should suuceed.
        options.Force = TRUE;
        VERIFY_SUCCEEDED(session->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>(), nullptr));
        VERIFY_IS_TRUE(deletedImages.size() > 0);
        VERIFY_IS_TRUE(std::strlen(deletedImages[0].Image) > 0);

        // Verify that the image is no longer in the list of images.
        ExpectImagePresent(*session, "alpine:latest", false);

        // Test delete failed if image not exists.
        VERIFY_ARE_EQUAL(
            WSLA_E_IMAGE_NOT_FOUND, session->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>(), nullptr));
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            auto settings = GetDefaultSessionSettings();
            settings.DmesgOutput = (ULONG) reinterpret_cast<ULONG_PTR>(write.get());
            WI_SetFlagIf(settings.FeatureFlags, WslaFeatureFlagsEarlyBootDmesg, earlyBootLogging);

            std::vector<char> dmesgContent;
            auto readDmesg = [read = read.get(), &dmesgContent]() mutable {
                DWORD Offset = 0;

                constexpr auto bufferSize = 1024;
                while (true)
                {
                    dmesgContent.resize(Offset + bufferSize);

                    DWORD Read{};
                    if (!ReadFile(read, &dmesgContent[Offset], bufferSize, &Read, nullptr))
                    {
                        LogInfo("ReadFile() failed: %lu", GetLastError());
                    }

                    if (Read == 0)
                    {
                        break;
                    }

                    Offset += Read;
                }
            };

            std::thread thread(readDmesg); // Needs to be created before the VM starts, to avoid a pipe deadlock.

            auto session = CreateSession(settings);
            auto detach = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                session.reset();
                if (thread.joinable())
                {
                    thread.join();
                }
            });

            write.reset();

            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo DmesgTest > /dev/kmsg"}, 0);

            session.reset();
            detach.reset();

            auto contentString = std::string(dmesgContent.begin(), dmesgContent.end());

            VERIFY_ARE_NOT_EQUAL(contentString.find("Run /init as init process"), std::string::npos);
            VERIFY_ARE_NOT_EQUAL(contentString.find("DmesgTest"), std::string::npos);

            return contentString;
        };

        auto validateFirstDmesgLine = [](const std::string& dmesg, const char* expected) {
            auto firstLf = dmesg.find("\n");
            VERIFY_ARE_NOT_EQUAL(firstLf, std::string::npos);
            VERIFY_IS_TRUE(dmesg.find(expected) < firstLf);
        };

        // Dmesg without early boot logging
        {
            auto dmesg = createVmWithDmesg(false);

            // Verify that the first line is "brd: module loaded";
            validateFirstDmesgLine(dmesg, "brd: module loaded");
        }

        // Dmesg with early boot logging
        {
            auto dmesg = createVmWithDmesg(true);
            validateFirstDmesgLine(dmesg, "Linux version");
        }
    }

    TEST_METHOD(TerminationCallback)
    {
        WSL2_TEST_ONLY();

        class DECLSPEC_UUID("7BC4E198-6531-4FA6-ADE2-5EF3D2A04DFF") CallbackInstance
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback, IFastRundown>
        {

        public:
            CallbackInstance(std::function<void(WSLAVirtualMachineTerminationReason, LPCWSTR)>&& callback) :
                m_callback(std::move(callback))
            {
            }

            HRESULT OnTermination(WSLAVirtualMachineTerminationReason Reason, LPCWSTR Details) override
            {
                m_callback(Reason, Details);
                return S_OK;
            }

        private:
            std::function<void(WSLAVirtualMachineTerminationReason, LPCWSTR)> m_callback;
        };

        std::promise<std::pair<WSLAVirtualMachineTerminationReason, std::wstring>> promise;

        CallbackInstance callback{[&](WSLAVirtualMachineTerminationReason reason, LPCWSTR details) {
            promise.set_value(std::make_pair(reason, details));
        }};

        WSLA_SESSION_SETTINGS sessionSettings = GetDefaultSessionSettings();
        sessionSettings.TerminationCallback = &callback;

        auto session = CreateSession(sessionSettings);

        session.reset();
        auto future = promise.get_future();
        auto result = future.wait_for(std::chrono::seconds(30));
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, WSLAVirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        WSLAProcessLauncher launcher("/bin/sh", {"/bin/sh"}, {"TERM=xterm-256color"}, ProcessFlags::None);
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 0, .Type = WSLAFdTypeTerminalInput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 1, .Type = WSLAFdTypeTerminalOutput});
        launcher.AddFd(WSLA_PROCESS_FD{.Fd = 2, .Type = WSLAFdTypeTerminalControl});

        auto process = launcher.Launch(*session);

        wil::unique_handle ttyInput = process.GetStdHandle(0);
        wil::unique_handle ttyOutput = process.GetStdHandle(1);

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(ttyOutput.get(), buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(ttyInput.get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("\033[?2004hsh-5.2# ");
        writeTty("echo OK\n");
        validateTtyOutput("echo OK\r\n\033[?2004l\rOK");

        // Exit the shell
        writeTty("exit\n");

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 1000));
    }

    TEST_METHOD(NATNetworking)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);
    }

    TEST_METHOD(NATNetworkingWithDnsTunneling)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;
        WI_SetFlag(settings.FeatureFlags, WslaFeatureFlagsDnsTunneling);

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        // Verify that /etc/resolv.conf is correctly configured.
        auto result = ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver ", "/etc/resolv.conf"}, 0);

        VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
    }

    TEST_METHOD(VirtioProxyNetworking)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeVirtioProxy;

        auto session = CreateSession(settings);

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);
    }

    TEST_METHOD(OpenFiles)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        struct FileFd
        {
            int Fd;
            WSLAFdType Flags;
            const char* Path;
        };

        auto createProcess = [&](const std::vector<std::string>& Args, const std::vector<FileFd>& Fds, HRESULT expectedError = S_OK) {
            WSLAProcessLauncher launcher(Args[0], Args, {}, ProcessFlags::None);

            for (const auto& e : Fds)
            {
                launcher.AddFd(WSLA_PROCESS_FD{.Fd = e.Fd, .Type = e.Flags, .Path = e.Path});
            }

            auto [hresult, _, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, expectedError);

            return std::move(process);
        };

        {
            auto process =
                createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WSLAFdTypeDefault, nullptr}});

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Output[1], "cat\n");
        }

        {

            auto read = [&]() {
                auto process =
                    createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileInput, "/tmp/output"}, {1, WSLAFdTypeDefault, nullptr}});
                return process->WaitAndCaptureOutput().Output[1];
            };

            // Write to a new file.
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr},
                 {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput | WSLAFdTypeLinuxFileCreate), "/tmp/output"}});

            constexpr auto content = "TestOutput";
            VERIFY_IS_TRUE(WriteFile(process->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));

            VERIFY_ARE_EQUAL(process->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);

            // Append content to the same file
            auto appendProcess = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr},
                 {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput | WSLAFdTypeLinuxFileAppend), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(appendProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(appendProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), std::format("{}{}", content, content));

            // Truncate the file
            auto truncProcess = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeDefault, nullptr}, {1, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), "/tmp/output"}});

            VERIFY_IS_TRUE(WriteFile(truncProcess->GetStdHandle(0).get(), content, static_cast<DWORD>(strlen(content)), nullptr, nullptr));
            VERIFY_ARE_EQUAL(truncProcess->WaitAndCaptureOutput().Code, 0);

            VERIFY_ARE_EQUAL(read(), content);
        }

        // Test various error paths
        {
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), "/tmp/DoesNotExist"}}, E_FAIL);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeDefault), "should-be-null"}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeDefault | WSLAFdTypeLinuxFileOutput), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
            createProcess({"/bin/cat"}, {{0, static_cast<WSLAFdType>(WSLAFdTypeLinuxFileInput | WSLAFdTypeLinuxFileAppend), nullptr}}, E_INVALIDARG);
        }

        // Validate that read & write modes are respected
        {
            auto process = createProcess(
                {"/bin/cat"},
                {{0, WSLAFdTypeLinuxFileInput, "/proc/self/comm"}, {1, WSLAFdTypeLinuxFileInput, "/tmp/output"}, {2, WSLAFdTypeDefault, nullptr}});

            auto result = process->WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[2], "/bin/cat: write error: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(result.Code, 1);
        }

        {
            auto process = createProcess({"/bin/cat"}, {{0, WSLAFdTypeLinuxFileOutput, "/tmp/output"}, {2, WSLAFdTypeDefault, nullptr}});
            auto result = process->WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(result.Output[2], "/bin/cat: standard output: Bad file descriptor\n");
            VERIFY_ARE_EQUAL(result.Code, 1);
        }
    }

    void WaitForOutput(HANDLE Handle, const char* Content)
    {
        std::string output;
        DWORD index = 0;
        while (true) // TODO: timeout
        {
            constexpr auto bufferSize = 100;
            output.resize(output.size() + bufferSize);
            DWORD bytesRead = 0;
            if (!ReadFile(Handle, &output[index], bufferSize, &bytesRead, nullptr))
            {
                LogError("ReadFile failed with %lu", GetLastError());
                VERIFY_FAIL();
            }
            output.resize(index + bytesRead);
            if (bytesRead == 0)
            {
                LogError("Process exited, output: %hs", output.c_str());
                VERIFY_FAIL();
            }

            index += bytesRead;
            if (output.find(Content) != std::string::npos)
            {
                break;
            }
        }
    }

    TEST_METHOD(NATPortMapping)
    {
        WSL2_TEST_ONLY();

        // TODO: Enable again once socat is available in the runtime VHD.
        LogSkipped("Skipping test since socat is required in the runtime VHD");
        return;

        auto settings = GetDefaultSessionSettings();
        settings.RootVhdOverride = testVhd.c_str(); // socat is required to run this test case.
        settings.RootVhdTypeOverride = "ext4";
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        auto listen = [&](short port, const char* content, bool ipv6) {
            auto cmd = std::format("echo -n '{}' | /usr/bin/socat -dd TCP{}-LISTEN:{},reuseaddr -", content, ipv6 ? "6" : "", port);
            auto process = WSLAProcessLauncher("/bin/sh", {"/bin/sh", "-c", cmd}).Launch(*session);
            WaitForOutput(process.GetStdHandle(2).get(), "listening on");

            return process;
        };

        auto connectAndRead = [&](short port, int family) -> std::string {
            SOCKADDR_INET addr{};
            addr.si_family = family;
            INETADDR_SETLOOPBACK((PSOCKADDR)&addr);
            SS_PORT(&addr) = htons(port);

            wil::unique_socket hostSocket{socket(family, SOCK_STREAM, IPPROTO_TCP)};
            THROW_LAST_ERROR_IF(!hostSocket);
            THROW_LAST_ERROR_IF(connect(hostSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);

            return ReadToString(hostSocket.get());
        };

        auto expectContent = [&](short port, int family, const char* expected) {
            auto content = connectAndRead(port, family);
            VERIFY_ARE_EQUAL(content, expected);
        };

        auto expectNotBound = [&](short port, int family) {
            auto result = wil::ResultFromException([&]() { connectAndRead(port, family); });

            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(WSAECONNREFUSED));
        };

        // Map port
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        // Validate that the same port can't be bound twice
        VERIFY_ARE_EQUAL(session->MapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Check simple case
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that same port mapping can be reused
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that the connection is immediately reset if the port is not bound on the linux side
        expectContent(1234, AF_INET, "");

        // Add a ipv6 binding
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1234, 80));

        // Validate that ipv6 bindings work as well.
        listen(80, "port80ipv6", true);
        expectContent(1234, AF_INET6, "port80ipv6");

        // Unmap the ipv4 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));

        // Verify that a proper error is returned if the mapping doesn't exist
        VERIFY_ARE_EQUAL(session->UnmapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        // Unmap the v6 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1234, 80));

        // Map another port as v6 only
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1235, 81));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1235, 81));
        VERIFY_ARE_EQUAL(session->UnmapVmPort(AF_INET6, 1235, 81), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        auto process =
            WSLAProcessLauncher{"/usr/bin/socat", {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"}}
                .Launch(*session);

        WaitForOutput(process.GetStdHandle(2).get(), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        // Create a 'stuck' process
        auto process = WSLAProcessLauncher{"/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout}.Launch(*session);

        // Stop the service
        StopWslaService();
    }

    void ValidateWindowsMounts(bool enableVirtioFs)
    {
        auto settings = GetDefaultSessionSettings();
        WI_SetFlagIf(settings.FeatureFlags, WslaFeatureFlagsVirtioFs, enableVirtioFs);

        auto session = CreateSession(settings);

        auto expectedMountOptions = [&](bool readOnly) -> std::string {
            if (enableVirtioFs)
            {
                return std::format("/win-path*virtiofs*{},relatime*", readOnly ? "ro" : "rw");
            }
            else
            {
                return std::format(
                    "/win-path*9p*{},relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*", readOnly ? "ro" : "rw");
            }
        };

        auto testFolder = std::filesystem::current_path() / "test-folder";
        std::filesystem::create_directories(testFolder);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        // Validate writeable mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(false));

            // Validate that mount can't be stacked on each other
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that folder is writeable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt && sync"}, 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Validate that folder is not writeable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate various error paths
        {
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"relative-path", "/win-path", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"C:\\does-not-exist", "/win-path", true), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/not-mounted"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/proc"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
        }
    }

    TEST_METHOD(WindowsMounts)
    {
        WSL2_TEST_ONLY();
        ValidateWindowsMounts(false);
    }

    TEST_METHOD(WindowsMountsVirtioFs)
    {
        WSL2_TEST_ONLY();
        ValidateWindowsMounts(true);
    }

    // This test case validates that no file descriptors are leaked to user processes.
    TEST_METHOD(Fd)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();
        auto result =
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo /proc/self/fd/* && (readlink -v /proc/self/fd/* || true)"}, 0);

        // Note: fd/0 is opened by readlink to read the actual content of /proc/self/fd.
        if (!PathMatchSpecA(result.Output[1].c_str(), "/proc/self/fd/0 /proc/self/fd/1 /proc/self/fd/2\nsocket:*\nsocket:*"))
        {
            LogInfo("Found additional fds: %hs", result.Output[1].c_str());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(GPU)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings();
        WI_SetFlag(settings.FeatureFlags, WslaFeatureFlagsGPU);

        auto session = CreateSession(settings);

        // Validate that the GPU device is available.
        ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -c /dev/dxg"}, 0);

        ExpectMount(
            session.get(),
            "/usr/lib/wsl/drivers",
            "/usr/lib/wsl/drivers*9p*relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

        ExpectMount(
            session.get(), "/usr/lib/wsl/lib", "/usr/lib/wsl/lib none*overlay ro,relatime,lowerdir=/usr/lib/wsl/lib/packaged*");

        // Validate that the mount points are not writeable.
        VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}).Code, 1L);
        VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}).Code, 1L);

        // Validate that trying to mount the shares without GPU support disabled fails.
        {
            session.reset(); // Required to close the storage VHD.

            WI_ClearFlag(settings.FeatureFlags, WslaFeatureFlagsGPU);
            session = CreateSession(settings);

            // Validate that the GPU device is not available.
            ExpectMount(session.get(), "/usr/lib/wsl/drivers", {});
            ExpectMount(session.get(), "/usr/lib/wsl/lib", {});
        }
    }

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        // Sanity check.
        ExpectCommandResult(session.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(session.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(session.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    TEST_METHOD(PmemVhds)
    {
        WSL2_TEST_ONLY();

        // Test with SCSI boot VHDs.
        {
            auto settings = GetDefaultSessionSettings();
            WI_ClearFlag(settings.FeatureFlags, WslaFeatureFlagsPmemVhds);

            auto session = CreateSession(settings);

            // Validate that SCSI devices are present and PMEM devices are not.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/sda"}, 0);
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/sdb"}, 0);
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/pmem0"}, 1);
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/pmem1"}, 1);

            // Verify that the SCSI device is readable.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "dd if=/dev/sda of=/dev/null bs=512 count=1 2>&1"}, 0);
        }

        // Test with PMEM boot VHDs enabled.
        {
            auto settings = GetDefaultSessionSettings();
            WI_SetFlag(settings.FeatureFlags, WslaFeatureFlagsPmemVhds);

            auto session = CreateSession(settings);

            // Validate that PMEM devices are present.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/pmem0"}, 0);
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -b /dev/pmem1"}, 0);

            // Verify that the PMEM devices can be read from.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "dd if=/dev/pmem0 of=/dev/null bs=512 count=1 2>&1"}, 0);
        }
    }

    TEST_METHOD(CreateRootNamespaceProcess)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        // Simple case
        {
            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo OK"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Output[2], "");
        }

        // Stdout + stderr
        {

            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo stdout && (echo stderr 1>& 2)"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "stdout\n");
            VERIFY_ARE_EQUAL(result.Output[2], "stderr\n");
        }

        // Write a large stdin buffer and expect it back on stdout.
        {
            std::vector<char> largeBuffer;
            std::string pattern = "ExpectedBufferContent";

            for (size_t i = 0; i < 1024 * 1024; i++)
            {
                largeBuffer.insert(largeBuffer.end(), pattern.begin(), pattern.end());
            }

            WSLAProcessLauncher launcher(
                "/bin/sh", {"/bin/sh", "-c", "cat && (echo completed 1>& 2)"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(process.GetStdHandle(0), largeBuffer));
            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_IS_TRUE(std::equal(largeBuffer.begin(), largeBuffer.end(), result.Output[1].begin(), result.Output[1].end()));
            VERIFY_ARE_EQUAL(result.Output[2], "completed\n");
        }

        // Create a stuck process and kill it.
        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            // Try to send invalid signal to the process
            VERIFY_ARE_EQUAL(process.Get().Signal(9999), E_FAIL);

            // Send SIGKILL(9) to the process.
            VERIFY_SUCCEEDED(process.Get().Signal(WSLASignalSIGKILL));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, WSLASignalSIGKILL + 128);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            // Validate that process can't be signalled after it exited.
            VERIFY_ARE_EQUAL(process.Get().Signal(WSLASignalSIGKILL), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Validate that errno is correctly propagated
        {
            WSLAProcessLauncher launcher("doesnotexist", {});

            auto [hresult, error, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 2); // ENOENT
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/", {});

            auto [hresult, error, process] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 13); // EACCESS
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);
            auto dummyHandle = process.GetStdHandle(1);

            // Verify that the same handle can only be acquired once.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(1, reinterpret_cast<ULONG*>(&dummyHandle)), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that trying to acquire a std handle that doesn't exist fails as expected.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(3, reinterpret_cast<ULONG*>(&dummyHandle)), E_INVALIDARG);

            // Validate that the process object correctly handle requests after the VM has terminated.
            session.reset();
            VERIFY_ARE_EQUAL(process.Get().Signal(WSLASignalSIGKILL), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    TEST_METHOD(CrashDumpCollection)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();
        int processId = 0;

        // Cache the existing crash dumps so we can check that a new one is created.
        auto crashDumpsDir = std::filesystem::temp_directory_path() / "wsla-crashes";
        std::set<std::filesystem::path> existingDumps;

        if (std::filesystem::exists(crashDumpsDir))
        {
            existingDumps = {std::filesystem::directory_iterator(crashDumpsDir), std::filesystem::directory_iterator{}};
        }

        // Create a stuck process and crash it.
        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto process = launcher.Launch(*session);

            // Get the process id. This is need to identify the crash dump file.
            VERIFY_SUCCEEDED(process.Get().GetPid(&processId));

            // Send SIGSEV(11) to crash the process.
            VERIFY_SUCCEEDED(process.Get().Signal(WSLASignalSIGSEGV));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 128 + WSLASignalSIGSEGV);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            VERIFY_ARE_EQUAL(process.Get().Signal(WSLASignalSIGKILL), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Dumps files are named with the format: wsl-crash-<sessionId>-<pid>-<processname>-<code>.dmp
        // Check if a new file was added in crashDumpsDir matching the pattern and not in existingDumps.
        std::string expectedPattern = std::format("wsl-crash-*-{}-_usr_bin_cat-11.dmp", processId);

        auto dumpFile = wsl::shared::retry::RetryWithTimeout<std::filesystem::path>(
            [crashDumpsDir, expectedPattern, existingDumps]() {
                for (const auto& entry : std::filesystem::directory_iterator(crashDumpsDir))
                {
                    const auto& filePath = entry.path();
                    if (existingDumps.find(filePath) == existingDumps.end() &&
                        PathMatchSpecA(filePath.filename().string().c_str(), expectedPattern.c_str()))
                    {
                        return filePath;
                    }
                }

                throw wil::ResultException(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            },
            std::chrono::milliseconds{100},
            std::chrono::seconds{10});

        // Ensure that the dump file is cleaned up after test completion.
        auto cleanup = wil::scope_exit([&] {
            if (std::filesystem::exists(dumpFile))
            {
                std::filesystem::remove(dumpFile);
            }
        });

        VERIFY_IS_TRUE(std::filesystem::exists(dumpFile));
        VERIFY_IS_TRUE(std::filesystem::file_size(dumpFile) > 0);
    }

    TEST_METHOD(VhdFormatting)
    {
        WSL2_TEST_ONLY();

        auto session = CreateSession();

        constexpr auto formatedVhd = L"test-format-vhd.vhdx";

        // TODO: Replace this by a proper SDK method once it exists
        auto tokenInfo = wil::get_token_information<TOKEN_USER>();
        wsl::core::filesystem::CreateVhd(formatedVhd, 100 * 1024 * 1024, tokenInfo->User.Sid, false, false);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            session.reset();
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(formatedVhd));
        });

        // Format the disk.
        auto absoluteVhdPath = std::filesystem::absolute(formatedVhd).wstring();
        VERIFY_SUCCEEDED(session->FormatVirtualDisk(absoluteVhdPath.c_str()));

        // Validate error paths.
        VERIFY_ARE_EQUAL(session->FormatVirtualDisk(L"DoesNotExist.vhdx"), E_INVALIDARG);
        VERIFY_ARE_EQUAL(session->FormatVirtualDisk(L"C:\\DoesNotExist.vhdx"), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    TEST_METHOD(CreateContainer)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        // Test a simple container start.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-simple", "echo", {"OK"});
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});
        }

        // Validate that env is correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-env", "/bin/sh", {"-c", "echo $testenv"}, {{"testenv=testvalue"}});
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that exit codes are correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-exit-code", "/bin/sh", {"-c", "exit 12"});
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {}, 12);
        }

        // Validate that stdin is correctly wired
        {
            WSLAContainerLauncher launcher(
                "debian:latest",
                "test-default-entrypoint",
                "/bin/cat",
                {},
                {},
                WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST,
                ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto container = launcher.Launch(*session);

            auto process = container.GetInitProcess();
            auto input = process.GetStdHandle(0);

            std::string shellInput = "foo";
            std::vector<char> inputBuffer{shellInput.begin(), shellInput.end()};

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(std::move(input), inputBuffer));

            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_ARE_EQUAL(result.Output[2], "");
            VERIFY_ARE_EQUAL(result.Output[1], "foo");
        }

        // Validate that stdin behaves correctly if closed without any input.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-stdin", "/bin/cat");
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();
            process.GetStdHandle(0); // Close stdin;

            ValidateProcessOutput(process, {{1, ""}});
        }

        // Validate error paths
        {
            WSLAContainerLauncher launcher("debian:latest", std::string(WSLA_MAX_CONTAINER_NAME_LENGTH + 1, 'a'), "/bin/cat");
            auto [hresult, container] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher(std::string(WSLA_MAX_IMAGE_NAME_LENGTH + 1, 'a'), "dummy", "/bin/cat");
            auto [hresult, container] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher("invalid-image-name", "dummy", "/bin/cat");
            auto [hresult, container] = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, WSLA_E_IMAGE_NOT_FOUND);
        }

        // Test null image name
        {
            WSLA_CONTAINER_OPTIONS options{};
            options.Image = nullptr;
            options.Name = "test-container";
            options.InitProcessOptions.CommandLine = nullptr;
            options.InitProcessOptions.CommandLineCount = 0;

            wil::com_ptr<IWSLAContainer> container;
            auto hr = session->CreateContainer(&options, &container, nullptr);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test null container name
        {
            WSLA_CONTAINER_OPTIONS options{};
            options.Image = "debian:latest";
            options.Name = nullptr;
            options.InitProcessOptions.CommandLine = nullptr;
            options.InitProcessOptions.CommandLineCount = 0;

            wil::com_ptr<IWSLAContainer> container;
            VERIFY_SUCCEEDED(session->CreateContainer(&options, &container, nullptr));
            VERIFY_SUCCEEDED(container->Delete());
        }
    }

    TEST_METHOD(ContainerState)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLA_CONTAINER_STATE>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;

            VERIFY_SUCCEEDED(session->ListContainers(&containers, containers.size_address<ULONG>()));
            VERIFY_ARE_EQUAL(expectedContainers.size(), containers.size());

            for (size_t i = 0; i < expectedContainers.size(); i++)
            {
                const auto& [expectedName, expectedImage, expectedState] = expectedContainers[i];
                VERIFY_ARE_EQUAL(expectedName, containers[i].Name);
                VERIFY_ARE_EQUAL(expectedImage, containers[i].Image);
                VERIFY_ARE_EQUAL(expectedState, containers[i].State);
            }
        };

        {
            // Validate that the container list is initially empty.
            expectContainerList({});

            // Start one container and wait for it to exit.
            {
                WSLAContainerLauncher launcher("debian:latest", "exited-container", "echo", {"OK"});
                auto container = launcher.Launch(*session);
                auto process = container.GetInitProcess();

                ValidateProcessOutput(process, {{1, "OK\n"}});
                expectContainerList({{"exited-container", "debian:latest", WslaContainerStateExited}});
            }

            // Create a stuck container.
            WSLAContainerLauncher launcher("debian:latest", "test-container-1", {}, {"sleep", "99999"});

            auto container = launcher.Launch(*session);

            // Verify that the container is in running state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);
            expectContainerList({{"test-container-1", "debian:latest", WslaContainerStateRunning}});

            // Kill the container init process and expect it to be in exited state.
            auto initProcess = container.GetInitProcess();
            VERIFY_SUCCEEDED(initProcess.Get().Signal(WSLASignalSIGKILL));

            // Wait for the process to actually exit.
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    initProcess.GetExitCode(); // Throw if the process hasn't exited yet.
                },
                std::chrono::milliseconds{100},
                std::chrono::seconds{30});

            // Expect the container to be in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);
            expectContainerList({{"test-container-1", "debian:latest", WslaContainerStateExited}});

            // Open a new reference to the same container.
            wil::com_ptr<IWSLAContainer> sameContainer;
            VERIFY_SUCCEEDED(session->OpenContainer("test-container-1", &sameContainer));

            // Verify that the state matches.
            WSLA_CONTAINER_STATE state{};
            VERIFY_SUCCEEDED(sameContainer->GetState(&state));
            VERIFY_ARE_EQUAL(state, WslaContainerStateExited);

            VERIFY_SUCCEEDED(container.Get().Delete());
        }

        // Test StopContainer
        {
            // Create a container
            WSLAContainerLauncher launcher(
                "debian:latest", "test-container-2", "sleep", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto container = launcher.Launch(*session);

            // Verify that the container is in running state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(15, 0));

            // TODO: Once 'container run' is split into 'container create' + 'container start',
            // validate that Stop() on a container in 'Created' state returns ERROR_INVALID_STATE.
            expectContainerList({{"test-container-2", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());
            expectContainerList({});
        }

        // Verify that trying to open a non existing container fails.
        {
            wil::com_ptr<IWSLAContainer> sameContainer;
            VERIFY_ARE_EQUAL(session->OpenContainer("does-not-exist", &sameContainer), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Validate that container names are unique.
        {
            WSLAContainerLauncher launcher(
                "debian:latest", "test-unique-name", "sleep", {"99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto container = launcher.Launch(*session);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Validate that a container with the same name cannot be started
            VERIFY_ARE_EQUAL(
                WSLAContainerLauncher("debian:latest", "test-unique-name", "echo", {"OK"}).LaunchNoThrow(*session).first,
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that running containers can't be deleted.
            VERIFY_ARE_EQUAL(container.Get().Delete(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Kill the container.
            auto initProcess = container.GetInitProcess();
            initProcess.Get().Signal(WSLASignalSIGKILL);

            // Wait for the process to actually exit.
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    initProcess.GetExitCode(); // Throw if the process hasn't exited yet.
                },
                std::chrono::milliseconds{100},
                std::chrono::seconds{30});

            expectContainerList({{"test-unique-name", "debian:latest", WslaContainerStateExited}});

            // Verify that calling Stop() on exited containers is a no-op and state remains as WslaContainerStateExited.
            VERIFY_SUCCEEDED(container.Get().Stop(15, 0));
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that stopped containers can be deleted.
            VERIFY_SUCCEEDED(container.Get().Delete());

            // Verify that stopping a deleted container returns ERROR_INVALID_STATE.
            VERIFY_ARE_EQUAL(container.Get().Stop(15, 0), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers can't be deleted again.
            VERIFY_ARE_EQUAL(container.Get().Delete(), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers don't show up in the container list.
            expectContainerList({});

            // Verify that the same name can be reused now that the container is deleted.
            WSLAContainerLauncher otherLauncher(
                "debian:latest",
                "test-unique-name",
                "echo",
                {"OK"},
                {},
                WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto result = otherLauncher.Launch(*session).GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Validate that creating and starting a container separately behaves as expected

        {
            WSLAContainerLauncher launcher("debian:latest", "test-create", "sleep", {"99999"}, {});
            auto [result, container] = launcher.CreateNoThrow(*session);
            VERIFY_SUCCEEDED(result);

            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateCreated);
            VERIFY_SUCCEEDED(container->Get().Start());

            // Verify that Start() can't be called again on a running container.
            VERIFY_ARE_EQUAL(container->Get().Start(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateRunning);

            VERIFY_SUCCEEDED(container->Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateExited);

            VERIFY_SUCCEEDED(container->Get().Delete());

            WSLA_CONTAINER_STATE state{};
            VERIFY_ARE_EQUAL(container->Get().GetState(&state), RPC_E_DISCONNECTED);
        }

        // Validate that containers behave correctly if they outlive their session.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-dangling-ref", "sleep", {"99999"}, {});
            auto container = launcher.Launch(*session);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Delete the container to avoid leaving it dangling after test completion.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());

            // Terminate the session
            session.reset();

            // Validate that calling into the container returns RPC_E_DISCONNECTED.
            WSLA_CONTAINER_STATE state = WslaContainerStateRunning;
            VERIFY_ARE_EQUAL(container.Get().GetState(&state), RPC_E_DISCONNECTED);
            VERIFY_ARE_EQUAL(state, WslaContainerStateInvalid);
        }
    }

    TEST_METHOD(ContainerNetwork)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLA_CONTAINER_STATE>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;

            VERIFY_SUCCEEDED(session->ListContainers(&containers, containers.size_address<ULONG>()));
            VERIFY_ARE_EQUAL(expectedContainers.size(), containers.size());

            for (size_t i = 0; i < expectedContainers.size(); i++)
            {
                const auto& [expectedName, expectedImage, expectedState] = expectedContainers[i];
                VERIFY_ARE_EQUAL(expectedName, containers[i].Name);
                VERIFY_ARE_EQUAL(expectedImage, containers[i].Image);
                VERIFY_ARE_EQUAL(expectedState, containers[i].State);
            }
        };

        // Verify that containers launch successfully when host and none are used as network modes
        // TODO: Test bridge network container launch when VHD with bridge cni is ready
        // TODO: Add port mapping related tests when port mapping is implemented
        {
            WSLAContainerLauncher launcher(
                "debian:latest",
                "test-network",
                {},
                {"sleep", "99999"},
                {},
                WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto container = launcher.Launch(*session);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            auto details = container.Inspect();
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "host");

            VERIFY_SUCCEEDED(container.Get().Stop(15, 0));

            expectContainerList({{"test-network", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());

            expectContainerList({});
        }

        {
            WSLAContainerLauncher launcher(
                "debian:latest",
                "test-network",
                {},
                {"sleep", "99999"},
                {},
                WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto container = launcher.Launch(*session);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "none");

            VERIFY_SUCCEEDED(container.Get().Stop(15, 0));

            expectContainerList({{"test-network", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());

            expectContainerList({});
        }

        {
            WSLAContainerLauncher launcher(
                "debian:latest",
                "test-network",
                {},
                {"sleep", "99999"},
                {},
                (WSLA_CONTAINER_NETWORK_TYPE)6, // WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto retVal = launcher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(retVal.first, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher(
                "debian:latest",
                "test-network",
                {},
                {"sleep", "99999"},
                {},
                WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_BRIDGE,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            auto container = launcher.Launch(*session);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);
            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "bridge");

            VERIFY_SUCCEEDED(container.Get().Stop(15, 0));

            expectContainerList({{"test-network", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());

            expectContainerList({});
        }
    }

    TEST_METHOD(Exec)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        // Create a container.
        WSLAContainerLauncher launcher(
            "debian:latest",
            "test-container-exec",
            {},
            {"sleep", "99999"},
            {},
            WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE,
            ProcessFlags::Stdout | ProcessFlags::Stderr);

        auto container = launcher.Launch(*session);

        // Simple exec case.
        {
            auto process =
                WSLAProcessLauncher("/bin/echo", {"echo", "OK"}, {}, ProcessFlags::Stdout | ProcessFlags::Stderr).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "OK\n"}});
        }

        // Validate that stdin is correctly wired.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr)
                               .Launch(container.Get());

            std::string shellInput = "foo";
            std::vector<char> inputBuffer{shellInput.begin(), shellInput.end()};

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(process.GetStdHandle(0), inputBuffer));

            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_ARE_EQUAL(result.Output[2], "");
            VERIFY_ARE_EQUAL(result.Output[1], "foo");
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Validate that behavior is correct when stdin is closed without any input.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr)
                               .Launch(container.Get());

            process.GetStdHandle(0); // Close stdin.
            ValidateProcessOutput(process, {{1, ""}, {2, ""}});
        }

        // Validate that exit codes are correctly wired.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/sh", "-c", "exit 12"}, {}).Launch(container.Get());
            ValidateProcessOutput(process, {}, 12);
        }

        // Validate that environment is correctly wired.
        {
            auto process =
                WSLAProcessLauncher({}, {"/bin/sh", "-c", "echo $testenv"}, {{"testenv=testvalue"}}, ProcessFlags::Stdout | ProcessFlags::Stderr)
                    .Launch(container.Get());

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that an exec'd command returns when the container is stopped.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, ProcessFlags::Stdin | ProcessFlags::Stdout | ProcessFlags::Stderr)
                               .Launch(container.Get());

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 128 + WSLASignalSIGKILL);
        }

        // Validate error paths
        {
            // Validate that processes can't be launched in stopped containers.
            auto [result, _, __] = WSLAProcessLauncher({}, {"/bin/cat"}).LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // TODO: Implement proper handling of executables that don't exist in the container.
        }
    }

    void ExpectHttpResponse(LPCWSTR Url, std::optional<int> expectedCode)
    {
        const winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
        filter.CacheControl().WriteBehavior(winrt::Windows::Web::Http::Filters::HttpCacheWriteBehavior::NoCache);

        const winrt::Windows::Web::Http::HttpClient client(filter);

        try
        {
            auto response = client.GetAsync(winrt::Windows::Foundation::Uri(Url)).get();
            auto content = response.Content().ReadAsStringAsync().get();

            if (expectedCode.has_value())
            {
                VERIFY_ARE_EQUAL(static_cast<int>(response.StatusCode()), expectedCode.value());
            }
            else
            {
                LogError("Unexpected reply for: %ls", Url);
                VERIFY_FAIL();
            }
        }
        catch (...)
        {
            auto result = wil::ResultFromCaughtException();

            if (!expectedCode.has_value())
            {
                // We currently reset the connection if connect() fails inside the VM. Consider failing the Windows connect() instead.
                VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(WININET_E_INVALID_SERVER_RESPONSE));
            }
            else
            {
                LogError("Expected success but request failed with 0x%08X for: %ls", result, Url);
                VERIFY_FAIL();
            }
        }
    }

    void RunPortMappingsTest(WSLA_CONTAINER_NETWORK_TYPE Mode)
    {
        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;

        auto session = CreateSession(settings);

        auto expectBoundPorts = [&](RunningWSLAContainer& Container, const std::vector<std::string>& expectedBoundPorts) {
            auto ports = Container.Inspect().HostConfig.PortBindings;

            std::vector<std::string> boundPorts;

            for (const auto& e : ports)
            {
                boundPorts.emplace_back(e.first);
            }

            if (!std::ranges::equal(boundPorts, expectedBoundPorts))
            {
                LogError(
                    "Port bindings do not match expected values. Expected: [%hs], Actual: [%hs]",
                    wsl::shared::string::Join(expectedBoundPorts, ',').c_str(),
                    wsl::shared::string::Join(boundPorts, ',').c_str());

                VERIFY_FAIL();
            }
        };

        // Test a simple port mapping.
        {
            WSLAContainerLauncher launcher(
                "python:3.12-alpine", "test-ports", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6);

            auto container = launcher.Launch(*session);
            auto initProcess = container.GetInitProcess();
            auto stdoutHandle = initProcess.GetStdHandle(1);

            // Wait for the container bind() to be completed.
            WaitForOutput(stdoutHandle.get(), "Serving HTTP on 0.0.0.0 port 8000");

            expectBoundPorts(container, {"8000/tcp"});

            ExpectHttpResponse(L"http://127.0.0.1:1234", 200);
            ExpectHttpResponse(L"http://[::1]:1234", {});

            // Validate that the port cannot be reused while the container is running.
            WSLAContainerLauncher subLauncher(
                "python:3.12-alpine", "test-ports-2", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);

            subLauncher.AddPort(1234, 8000, AF_INET);
            auto [hresult, newContainer] = subLauncher.LaunchNoThrow(*session);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());

            container.Reset(); // TODO: Re-think container lifetime management.

            // Validate that the port can be reused now that the container is stopped.
            {
                WSLAContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-3", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);

                launcher.AddPort(1234, 8000, AF_INET);

                auto container = launcher.Launch(*session);
                auto initProcess = container.GetInitProcess();
                auto stdoutHandle = initProcess.GetStdHandle(1);

                // Wait for the container bind() to be completed.
                WaitForOutput(stdoutHandle.get(), "Serving HTTP on 0.0.0.0 port 8000");

                expectBoundPorts(container, {"8000/tcp"});
                ExpectHttpResponse(L"http://127.0.0.1:1234", 200);

                VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
                VERIFY_SUCCEEDED(container.Get().Delete());
                container.Reset(); // TODO: Re-think container lifetime management.
            }
        }

        // Validate that the same host port can't be bound twice in the same Create() call.
        {
            WSLAContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET);

            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*session).first, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        }

        auto bindSocket = [](auto port) {
            wil::unique_socket socket(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            VERIFY_ARE_NOT_EQUAL(bind(socket.get(), (sockaddr*)&address, sizeof(address)), SOCKET_ERROR);
            return socket;
        };

        // Validate that Create() fails if the port is already bound.
        {
            auto boundSocket = bindSocket(1235);
            WSLAContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);

            launcher.AddPort(1235, 8000, AF_INET);
            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*session).first, HRESULT_FROM_WIN32(WSAEACCES));

            // Validate that Create() correctly cleans up bound ports after a port fails to map
            {
                WSLAContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-fail", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, Mode);
                launcher.AddPort(1236, 8000, AF_INET); // Should succeed
                launcher.AddPort(1235, 8000, AF_INET); // Should fail.

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*session).first, HRESULT_FROM_WIN32(WSAEACCES));

                // Validate that port 1234 is still available.
                VERIFY_IS_TRUE(!!bindSocket(1236));
            }
        }

        // TODO: Uncomment once ipv6 port mapping is supported.
        // Validate ipv6 port mapping
        /*{
            WSLAContainerLauncher launcher(
                "python:3.12-alpine",
                "test-ports-ipv6",
                {},
                {"python3", "-m", "http.server", "--bind", "::1"},
                {"PYTHONUNBUFFERED=1"},
                Mode,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6);

            auto container = launcher.Launch(*session);
            auto initProcess = container.GetInitProcess();
            auto stdoutHandle = initProcess.GetStdHandle(1);

            // Wait for the container bind() to be completed.
            WaitForOutput(stdoutHandle.get(), "Serving HTTP on ::1 port 8000");

            ExpectHttpResponse(L"http://localhost:1234", {});

            ExpectHttpResponse(L"http://[::1]:1234", 200);
        }*/
    }

    TEST_METHOD(PortMappingsBridged)
    {
        RunPortMappingsTest(WSLA_CONTAINER_NETWORK_BRIDGE);
    }

    TEST_METHOD(PortMappingsHost)
    {
        RunPortMappingsTest(WSLA_CONTAINER_NETWORK_HOST);
    }

    TEST_METHOD(PortMappingsNone)
    {
        // Validate that trying to map ports without network fails.
        WSLAContainerLauncher launcher(
            "python:3.12-alpine", "test-ports-fail", {}, {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, WSLA_CONTAINER_NETWORK_NONE);

        launcher.AddPort(1234, 8000, AF_INET);

        VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*CreateSession()).first, E_INVALIDARG);
    }

    void ValidateContainerVolumes(bool enableVirtioFs)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto hostFolder = std::filesystem::current_path() / "test-volume";
        auto hostFolderReadOnly = std::filesystem::current_path() / "test-volume-ro";
        auto storage = std::filesystem::current_path() / "storage";

        std::filesystem::create_directories(hostFolder);
        std::filesystem::create_directories(hostFolderReadOnly);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(hostFolderReadOnly, ec);
            std::filesystem::remove_all(storage, ec);
        });

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;
        WI_SetFlagIf(settings.FeatureFlags, WslaFeatureFlagsVirtioFs, enableVirtioFs);

        auto session = CreateSession(settings);

        // Validate both folders exist in the container and that the readonly one cannot be written to.
        std::string containerName = "test-container";
        std::string containerPath = "/volume";
        std::string containerReadOnlyPath = "/volume-ro";

        // Container init script to validate volumes are mounted correctly.
        const std::string script =
            "set -e; "

            // Test that volumes are available in the container
            "test -d " +
            containerPath +
            "; "
            "test -d " +
            containerReadOnlyPath +
            "; "

            // Test that the container cannot write to the read-only volume
            "if touch " +
            containerReadOnlyPath +
            "/.ro-test 2>/dev/null;"
            "then echo 'FAILED'; "
            "else echo 'OK'; "
            "fi ";

        WSLAContainerLauncher launcher("debian:latest", containerName, "/bin/sh", {"-c", script});
        launcher.AddVolume(hostFolder.wstring(), containerPath, false);
        launcher.AddVolume(hostFolderReadOnly.wstring(), containerReadOnlyPath, true);

        {
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK\n"}});

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);
            VERIFY_SUCCEEDED(container.Get().Delete());
        }

        // Validate that the volumes are not mounted after container exits.
        ExpectMount(session.get(), std::format("/mnt/wsla/{}/volumes/{}", containerName, 0), {});
        ExpectMount(session.get(), std::format("/mnt/wsla/{}/volumes/{}", containerName, 1), {});
    }

    TEST_METHOD(ContainerVolume)
    {
        ValidateContainerVolumes(false);
    }

    TEST_METHOD(ContainerVolumeVirtioFs)
    {
        ValidateContainerVolumes(true);
    }

    TEST_METHOD(ContainerVolumeUnmountAllFoldersOnError)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto hostFolder = std::filesystem::current_path() / "test-volume";
        auto storage = std::filesystem::current_path() / "storage";

        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(storage, ec);
        });

        auto settings = GetDefaultSessionSettings();
        settings.NetworkingMode = WSLANetworkingModeNAT;
        settings.StoragePath = storage.c_str();
        settings.MaximumStorageSizeMb = 1024;

        auto session = CreateSession(settings);

        // Create a container with a simple command.
        WSLAContainerLauncher launcher("debian:latest", "test-container", "/bin/echo", {"OK"});
        launcher.AddVolume(hostFolder.wstring(), "/volume", false);

        // Add a volume with an invalid (non-existing) host path
        launcher.AddVolume(L"does-not-exist", "/volume-invalid", false);

        auto [result, container] = launcher.LaunchNoThrow(*session);
        VERIFY_FAILED(result);

        // Verify that the first volume was mounted before the error occurred, then unmounted after failure.
        ExpectMount(session.get(), "/mnt/wsla/test-container/volumes/0", {});
    }

    TEST_METHOD(LineBasedReader)
    {
        auto runTest = [](bool Crlf, const std::string& Data, const std::vector<std::string>& ExpectedLines) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> lines;
            auto onData = [&](const gsl::span<char>& data) { lines.emplace_back(data.data(), data.size()); };

            wsl::windows::common::relay::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::relay::LineBasedReadHandle>(std::move(readPipe), std::move(onData), Crlf));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::relay::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < lines.size(); i++)
            {
                if (i >= ExpectedLines.size())
                {
                    LogError(
                        "Input: '%hs'. Line %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedLines[i] != lines[i])
                {
                    LogError(
                        "Input: '%hs'. Line %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str(),
                        EscapeString(lines[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedLines.size() != lines.size())
            {
                LogError(
                    "Input: '%hs', Number of lines do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedLines.size(),
                    lines.size());
                VERIFY_FAIL();
            }
        };

        runTest(false, "foo\nbar", {"foo", "bar"});
        runTest(false, "foo", {"foo"});
        runTest(false, "\n", {});
        runTest(false, "\n\n", {});
        runTest(false, "\n\r\n", {"\r"});
        runTest(false, "\n\nfoo\nbar", {"foo", "bar"});
        runTest(false, "foo\r\nbar", {"foo\r", "bar"});
        runTest(true, "foo\nbar", {"foo\nbar"});
        runTest(true, "foo\r\nbar", {"foo", "bar"});
        runTest(true, "foo\rbar\nbaz", {"foo\rbar\nbaz"});
        runTest(true, "\r", {"\r"});
    }

    TEST_METHOD(HTTPChunkReader)
    {
        auto runTest = [](const std::string& Data, const std::vector<std::string>& ExpectedChunk) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> chunks;
            auto onData = [&](const gsl::span<char>& data) { chunks.emplace_back(data.data(), data.size()); };

            wsl::windows::common::relay::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::relay::HTTPChunkBasedReadHandle>(std::move(readPipe), std::move(onData)));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::relay::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < ExpectedChunk.size(); i++)
            {
                if (i >= chunks.size())
                {
                    LogError(
                        "Input: '%hs': Chunk %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedChunk[i] != chunks[i])
                {
                    LogError(

                        "Input: '%hs': Chunk %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str(),
                        EscapeString(chunks[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedChunk.size() != chunks.size())
            {
                LogError(
                    "Input: '%hs', Number of chunks do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedChunk.size(),
                    chunks.size());
                VERIFY_FAIL();
            }
        };

        runTest("3\r\nfoo\r\n3\r\nbar", {"foo", "bar"});
        runTest("1\r\na\r\n\r\n", {"a"});

        runTest("c\r\nlf\nin\r\nchunk\r\n3\r\nEOF", {"lf\nin\r\nchunk", "EOF"});
        runTest("15\r\n\r\nchunkstartingwithlf\r\n3\r\nEOF", {"\r\nchunkstartingwithlf", "EOF"});

        // Validate that invalid chunk sizes fail
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid\r\nInvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4nolf", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4invalid\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid\n", {}); }), E_INVALIDARG);
    }

    TEST_METHOD(DockerIORelay)
    {
        using namespace wsl::windows::common::relay;

        auto runTest = [](const std::vector<char>& Input, const std::string& ExpectedStdout, const std::string& ExpectedStderr) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            auto [stderrRead, stderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            MultiHandleWait io;

            std::string readStdout;
            std::string readStderr;

            io.AddHandle(std::make_unique<DockerIORelayHandle>(std::move(readPipe), std::move(stdoutWrite), std::move(stderrWrite)));
            io.AddHandle(std::make_unique<WriteHandle>(std::move(writePipe), Input));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stdoutRead), [&](const auto& buffer) { readStdout.append(buffer.data(), buffer.size()); }));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stderrRead), [&](const auto& buffer) { readStderr.append(buffer.data(), buffer.size()); }));

            io.Run({});

            VERIFY_ARE_EQUAL(ExpectedStdout, readStdout);
            VERIFY_ARE_EQUAL(ExpectedStderr, readStderr);
        };

        auto insert = [](std::vector<char>& buffer, auto fd, const std::string& content) {
            DockerIORelayHandle::MultiplexedHeader header;
            header.Fd = fd;
            header.Length = htonl(static_cast<uint32_t>(content.size()));

            buffer.insert(buffer.end(), (char*)&header, ((char*)&header) + sizeof(header));
            buffer.insert(buffer.end(), content.begin(), content.end());
        };

        {
            std::vector<char> input;
            insert(input, 1, "foo");
            insert(input, 1, "bar");
            insert(input, 2, "stderr");
            insert(input, 2, "stderrAgain");
            insert(input, 1, "stdOutAgain");

            runTest(input, "foobarstdOutAgain", "stderrstderrAgain");
        }

        {
            std::vector<char> input;
            insert(input, 0, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }

        {
            std::vector<char> input;
            insert(input, 12, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }
    }

    TEST_METHOD(ContainerRecoveryFromStorage)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        std::string containerName = "test-container";

        // Phase 1: Create session and container, then stop the container
        {
            auto session = CreateSession();

            // Create and start a container
            WSLAContainerLauncher launcher("debian:latest", containerName.c_str(), "/bin/echo", {"OK"});

            auto container = launcher.Launch(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Stop the container so it can be recovered and deleted later
            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);
        }

        // Phase 2: Create new session from same storage, recover and delete container
        {
            auto session = CreateSession();

            // Try to open the container from the previous session
            wil::com_ptr<IWSLAContainer> recoveredContainer;
            VERIFY_SUCCEEDED(session->OpenContainer(containerName.c_str(), &recoveredContainer));

            // Verify container state
            WSLA_CONTAINER_STATE state{};
            VERIFY_SUCCEEDED(recoveredContainer->GetState(&state));
            VERIFY_ARE_EQUAL(state, WslaContainerStateExited);

            // Delete the container
            VERIFY_SUCCEEDED(recoveredContainer->Delete());

            // Verify container is no longer accessible
            wil::com_ptr<IWSLAContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Phase 3: Create new session from same storage, verify the container is not listed.
        {
            auto session = CreateSession();

            // Verify container is no longer accessible
            wil::com_ptr<IWSLAContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }
    }

    TEST_METHOD(SessionManagement)
    {
        WSL2_TEST_ONLY();

        wil::com_ptr<IWSLASessionManager> manager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

        auto expectSessions = [&](const std::vector<std::wstring>& expectedSessions) {
            wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
            VERIFY_SUCCEEDED(manager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            std::set<std::wstring> displayNames;
            for (const auto& e : sessions)
            {
                auto [_, inserted] = displayNames.insert(e.DisplayName);

                VERIFY_IS_TRUE(inserted);
            }

            for (const auto& e : expectedSessions)
            {
                auto it = displayNames.find(e);
                if (it == displayNames.end())
                {
                    LogError("Session not found: %ls", e.c_str());
                    VERIFY_FAIL();
                }

                displayNames.erase(it);
            }

            for (const auto& e : displayNames)
            {
                LogError("Unexpected session found: %ls", e.c_str());
                VERIFY_FAIL();
            }
        };

        auto create = [this](LPCWSTR Name, WSLASessionFlags Flags) {
            auto settings = GetDefaultSessionSettings();
            settings.DisplayName = Name;
            settings.NetworkingMode = WSLANetworkingModeNone;
            settings.StoragePath = nullptr;

            return CreateSession(settings, Flags);
        };

        // Validate that non-persistent sessions are dropped when released
        {
            auto session1 = create(L"session-1", WSLASessionFlagsNone);
            expectSessions({L"session-1"});

            session1.reset();
            expectSessions({});
        }

        // Validate that persistent sessions are only dropped when explicitly terminated.
        {
            auto session1 = create(L"session-1", WSLASessionFlagsPersistent);
            expectSessions({L"session-1"});

            session1.reset();
            expectSessions({L"session-1"});
            session1 = create(L"session-1", WSLASessionFlagsOpenExisting);

            VERIFY_SUCCEEDED(session1->Terminate());
            session1.reset();
            expectSessions({});
        }

        // Validate that sessions can be reopened by name.
        {
            auto session1 = create(L"session-1", WSLASessionFlagsPersistent);
            expectSessions({L"session-1"});

            session1.reset();
            expectSessions({L"session-1"});

            auto session1Copy =
                create(L"session-1", static_cast<WSLASessionFlags>(WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting));

            expectSessions({L"session-1"});

            // Verify that name conflicts are correctly handled.
            auto settings = GetDefaultSessionSettings();
            settings.DisplayName = L"session-1";

            wil::com_ptr<IWSLASession> session;
            VERIFY_ARE_EQUAL(manager->CreateSession(&settings, WSLASessionFlagsPersistent, &session), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(session1Copy->Terminate());
            expectSessions({});

            // Validate that a new session is created if WSLASessionFlagsOpenExisting is set and no match is found.
            auto session2 = create(L"session-2", static_cast<WSLASessionFlags>(WSLASessionFlagsOpenExisting));
        }

        // Validate that elevated session can't be opened by non-elevated tokens
        {
            auto elevatedSession = create(L"elevated-session", WSLASessionFlagsNone);

            auto nonElevatedToken = GetNonElevatedToken(TokenImpersonation);
            auto revert = wil::impersonate_token(nonElevatedToken.get());
            auto nonElevatedSession = create(L"non-elevated-session", WSLASessionFlagsNone);

            // Validate that non-elevated tokens can't open an elevated session.
            wil::com_ptr<IWSLASession> openedSession;
            ULONG elevatedId{};
            VERIFY_SUCCEEDED(elevatedSession->GetId(&elevatedId));
            VERIFY_ARE_EQUAL(manager->OpenSession(elevatedId, &openedSession), HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED));
            VERIFY_IS_FALSE(!!openedSession);

            // Validate that non-elevated tokens can open non-elevated sessions.
            ULONG nonElevatedId{};
            VERIFY_SUCCEEDED(nonElevatedSession->GetId(&nonElevatedId));
            VERIFY_SUCCEEDED(manager->OpenSession(nonElevatedId, &openedSession));
            VERIFY_IS_TRUE(!!openedSession);
        }
    }
};
