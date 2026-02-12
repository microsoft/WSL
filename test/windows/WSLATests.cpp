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
using wsl::windows::common::RunningWSLAContainer;
using wsl::windows::common::RunningWSLAProcess;
using wsl::windows::common::WSLAContainerLauncher;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

class WSLATests
{
    WSL_TEST_CLASS(WSLATests)

    wil::unique_couninitialize_call m_coinit = wil::CoInitializeEx();
    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WSLA_SESSION_SETTINGS m_defaultSessionSettings{};
    wil::com_ptr<IWSLASession> m_defaultSession;
    static inline auto c_testSessionName = L"wsla-test";

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        m_storagePath = std::filesystem::current_path() / "test-storage";
        m_defaultSessionSettings = GetDefaultSessionSettings(c_testSessionName, true, WSLANetworkingModeVirtioProxy);
        m_defaultSession = CreateSession(m_defaultSessionSettings);

        wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
        VERIFY_SUCCEEDED(m_defaultSession->ListImages(&images, images.size_address<ULONG>()));

        auto hasImage = [&](const std::string& imageName) {
            return std::ranges::any_of(
                images.get(), images.get() + images.size(), [&](const auto& e) { return e.Image == imageName; });
        };

        if (!hasImage("debian:latest"))
        {
            VERIFY_SUCCEEDED(m_defaultSession->PullImage("debian:latest", nullptr, nullptr));
        }

        if (!hasImage("python:3.12-alpine"))
        {
            VERIFY_SUCCEEDED(m_defaultSession->PullImage("python:3.12-alpine", nullptr, nullptr));
        }

        // Hacky way to delete all containers.
        // TODO: Replace with the --rm flag once available.
        ExpectCommandResult(m_defaultSession.get(), {"/usr/bin/docker", "container", "prune", "-f"}, 0);

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        m_defaultSession.reset();

        // Keep the VHD when running in -f mode, to speed up subsequent test runs.
        if (!g_fastTestRun && !m_storagePath.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(m_storagePath, error);
            if (error)
            {
                LogError("Failed to cleanup storage path %ws: %hs", m_storagePath.c_str(), error.message().c_str());
            }
        }

        return true;
    }

    WSLA_SESSION_SETTINGS GetDefaultSessionSettings(LPCWSTR Name, bool enableStorage = false, WSLANetworkingMode networkingMode = WSLANetworkingModeNone)
    {
        WSLA_SESSION_SETTINGS settings{};
        settings.DisplayName = Name;
        settings.CpuCount = 4;
        settings.MemoryMb = 2024;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = enableStorage ? m_storagePath.c_str() : nullptr;
        settings.MaximumStorageSizeMb = 1000; // 1GB.
        settings.NetworkingMode = networkingMode;

        return settings;
    }

    auto ResetTestSession()
    {
        m_defaultSession.reset();

        return wil::scope_exit([this]() { m_defaultSession = CreateSession(m_defaultSessionSettings); });
    }

    static wil::com_ptr<IWSLASessionManager> OpenSessionManager()
    {
        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        return sessionManager;
    }

    wil::com_ptr<IWSLASession> CreateSession(const WSLA_SESSION_SETTINGS& sessionSettings, WSLASessionFlags Flags = WSLASessionFlagsNone)
    {
        const auto sessionManager = OpenSessionManager();

        wil::com_ptr<IWSLASession> session;

        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        WSLASessionState state{};
        VERIFY_SUCCEEDED(session->GetState(&state));
        VERIFY_ARE_EQUAL(state, WSLASessionStateRunning);

        return session;
    }

    RunningWSLAContainer OpenContainer(IWSLASession* session, const std::string& name)
    {
        wil::com_ptr<IWSLAContainer> rawContainer;
        VERIFY_SUCCEEDED(session->OpenContainer(name.c_str(), &rawContainer));

        return RunningWSLAContainer(std::move(rawContainer), {});
    }

    TEST_METHOD(GetVersion)
    {
        WSL2_TEST_ONLY();

        wil::com_ptr<IWSLASessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        WSLA_VERSION version{};

        VERIFY_SUCCEEDED(sessionManager->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    static RunningWSLAProcess::ProcessResult RunCommand(IWSLASession* session, const std::vector<std::string>& command, int timeout = 600000)
    {
        WSLAProcessLauncher process(command[0], command);

        return process.Launch(*session).WaitAndCaptureOutput(timeout);
    }

    static RunningWSLAProcess::ProcessResult ExpectCommandResult(
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

    void ValidateProcessOutput(RunningWSLAProcess& process, const std::map<int, std::string>& expectedOutput, int expectedResult = 0, DWORD Timeout = INFINITE)
    {
        auto result = process.WaitAndCaptureOutput(Timeout);

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

        auto sessionManager = OpenSessionManager();

        // Act: list sessions
        {
            wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            // Assert
            VERIFY_ARE_EQUAL(sessions.size(), 1u);
            const auto& info = sessions[0];

            // SessionId is implementation detail (starts at 1), so we only assert DisplayName here.
            VERIFY_ARE_EQUAL(std::wstring(info.DisplayName), c_testSessionName);
        }

        // List multiple sessions.
        {
            auto session2 = CreateSession(GetDefaultSessionSettings(L"wsla-test-list-2"));

            wil::unique_cotaskmem_array_ptr<WSLA_SESSION_INFORMATION> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            VERIFY_ARE_EQUAL(sessions.size(), 2);

            std::vector<std::wstring> displayNames;
            for (const auto& e : sessions)
            {
                displayNames.push_back(e.DisplayName);
            }

            std::ranges::sort(displayNames);

            VERIFY_ARE_EQUAL(displayNames[0], c_testSessionName);
            VERIFY_ARE_EQUAL(displayNames[1], L"wsla-test-list-2");
        }
    }

    TEST_METHOD(OpenSessionByNameFindsExistingSession)
    {
        WSL2_TEST_ONLY();

        auto sessionManager = OpenSessionManager();

        // Act: open by the same display name
        wil::com_ptr<IWSLASession> opened;
        VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(c_testSessionName, &opened));
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

        auto found = std::ranges::find(tags, Image) != tags.end();
        if (Present != found)
        {
            LogError("Image presence check failed for image: %hs, images: %hs", Image, wsl::shared::string::Join(tags, ',').c_str());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(PullImage)
    {
        WSL2_TEST_ONLY();

        {
            VERIFY_SUCCEEDED(m_defaultSession->PullImage("hello-world:linux", nullptr, nullptr));

            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:linux");
            WSLAContainerLauncher launcher("hello-world:linux", "wsla-pull-image-container");

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        {
            std::wstring expectedError =
                L"pull access denied for does-not, repository does not exist or may require 'docker login': denied: requested "
                L"access to the resource is denied";

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("does-not:exist", nullptr, nullptr), WSLA_E_IMAGE_NOT_FOUND);
            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            VERIFY_ARE_EQUAL(expectedError, comError->Message.get());
        }
    }

    TEST_METHOD(ListImages)
    {
        WSL2_TEST_ONLY();

        // TODO: Add more test coverage once ListImages() is fully implemented.

        // Validate that images with multiple tags are correctly returned.
        ExpectImagePresent(*m_defaultSession, "debian:latest");

        ExpectCommandResult(m_defaultSession.get(), {"/usr/bin/docker", "tag", "debian:latest", "debian:test-list-images"}, 0);

        auto cleanup = wil::scope_exit([&]() {
            WSLA_DELETE_IMAGE_OPTIONS options{.Image = "debian:test-list-images", .Force = false, .NoPrune = false};

            wil::unique_cotaskmem_array_ptr<WSLA_DELETED_IMAGE_INFORMATION> deletedImages;
            VERIFY_SUCCEEDED(m_defaultSession->DeleteImage(&options, &deletedImages, deletedImages.size_address<ULONG>()));
        });

        ExpectImagePresent(*m_defaultSession, "debian:test-list-images");
        ExpectImagePresent(*m_defaultSession, "debian:latest");

        cleanup.reset();
        ExpectImagePresent(*m_defaultSession, "debian:test-list-images", false);
        ExpectImagePresent(*m_defaultSession, "debian:latest");
    }

    // TODO: Test that invalid tars are correctly handled.
    TEST_METHOD(LoadImage)
    {
        WSL2_TEST_ONLY();

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldSaved.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(m_defaultSession->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "hello-world:latest");
        WSLAContainerLauncher launcher("hello-world:latest", "wsla-load-image-container");

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
    }

    // TODO: Test that invalid tars are correctly handled.
    TEST_METHOD(ImportImage)
    {
        WSL2_TEST_ONLY();

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(
            m_defaultSession->ImportImage(HandleToULong(imageTarFileHandle.get()), "my-hello-world:test", nullptr, fileSize.QuadPart));

        ExpectImagePresent(*m_defaultSession, "my-hello-world:test");

        // Validate that containers can be started from the imported image.
        WSLAContainerLauncher launcher("my-hello-world:test", "wsla-import-image-container", {"/hello"});

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

        // Validate that ImportImage fails if no tag is passed
        {
            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(HandleToULong(imageTarFileHandle.get()), "my-hello-world", nullptr, fileSize.QuadPart), E_INVALIDARG);
        }
    }

    TEST_METHOD(DeleteImage)
    {
        WSL2_TEST_ONLY();

        // Prepare alpine image to delete.
        VERIFY_SUCCEEDED(m_defaultSession->PullImage("alpine:latest", nullptr, nullptr));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "alpine:latest");

        // Launch a container to ensure that image deletion fails when in use.
        WSLAContainerLauncher launcher(
            "alpine:latest", "test-delete-container-in-use", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

        auto container = launcher.Launch(*m_defaultSession);

        // Verify that the container is in running state.
        VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

        // Test delete failed if image in use.
        WSLA_DELETE_IMAGE_OPTIONS options{};
        options.Image = "alpine:latest";
        options.Force = FALSE;
        wil::unique_cotaskmem_array_ptr<WSLA_DELETED_IMAGE_INFORMATION> deletedImages;

        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
            m_defaultSession->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>()));

        // Force should suuceed.
        options.Force = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>()));
        VERIFY_IS_TRUE(deletedImages.size() > 0);
        VERIFY_IS_TRUE(std::strlen(deletedImages[0].Image) > 0);

        // Verify that the image is no longer in the list of images.
        ExpectImagePresent(*m_defaultSession, "alpine:latest", false);

        // Test delete failed if image not exists.
        VERIFY_ARE_EQUAL(
            WSLA_E_IMAGE_NOT_FOUND, m_defaultSession->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>()));
    }

    void ValidateCOMErrorMessage(const std::optional<std::wstring>& Expected)
    {
        auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();

        if (comError.has_value())
        {
            if (!Expected.has_value())
            {
                LogError("Unexpected COM error: '%ls'", comError->Message.get());
                VERIFY_FAIL();
            }

            VERIFY_ARE_EQUAL(Expected.value(), comError->Message.get());
        }
        else
        {
            if (Expected.has_value())
            {
                LogError("Expected COM error: '%ls' but none was set", Expected->c_str());
                VERIFY_FAIL();
            }
        }
    }

    TEST_METHOD(SaveImage)
    {
        WSL2_TEST_ONLY();
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldSaved.tar";
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            // Load the image from a saved tar
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            WSLAContainerLauncher launcher("hello-world:latest", "wsla-hello-world-container");
            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Save the image to a tar file.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
            wil::unique_handle imageTarFileHandle{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
            VERIFY_SUCCEEDED(m_defaultSession->SaveImage(HandleToULong(imageTarFileHandle.get()), "hello-world:latest", nullptr));
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, true);
        }

        // Load the saved image to verify it's valid.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            // Load the image from a saved tar
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            WSLAContainerLauncher launcher("hello-world:latest", "wsla-hello-world-container");
            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Try to save an invalid image.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldError.tar";
            wil::unique_handle imageTarFileHandle{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
            VERIFY_FAILED(m_defaultSession->SaveImage(HandleToULong(imageTarFileHandle.get()), "hello-wld:latest", nullptr));
            ValidateCOMErrorMessage(L"reference does not exist");

            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
        }
    }
    TEST_METHOD(ExportContainer)
    {
        WSL2_TEST_ONLY();
        // Load an image and launch a container to verify image is valid.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldSaved.tar";
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            WSLAContainerLauncher launcher("hello-world:latest", "wsla-hello-world-container");
            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

            // Export the container to a tar file.
            std::filesystem::path containerTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
            wil::unique_handle containerTarFileHandle{CreateFileW(
                containerTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == containerTarFileHandle.get());
            VERIFY_IS_TRUE(GetFileSizeEx(containerTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
            VERIFY_SUCCEEDED(m_defaultSession->ExportContainer(HandleToULong(containerTarFileHandle.get()), container.Id().c_str(), nullptr));
            VERIFY_IS_TRUE(GetFileSizeEx(containerTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, true);
        }

        // Load the exported container to verify it's valid.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(HandleToULong(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            WSLAContainerLauncher launcher("hello-world:latest", "wsla-hello-world-container");
            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Try to export a non-existing container.
        {
            std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExportError.tar";
            wil::unique_handle contTarFileHandle{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == contTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(contTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);

            VERIFY_ARE_EQUAL(m_defaultSession->ExportContainer(HandleToULong(contTarFileHandle.get()), "dummy", nullptr), WSLA_E_CONTAINER_NOT_FOUND);
            ValidateCOMErrorMessage(L"No such container: dummy");

            VERIFY_IS_TRUE(GetFileSizeEx(contTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
        }
    }

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            auto settings = GetDefaultSessionSettings(L"dmesg-output-test");
            settings.DmesgOutput = (ULONG) reinterpret_cast<ULONG_PTR>(write.get());
            WI_UpdateFlag(settings.FeatureFlags, WslaFeatureFlagsEarlyBootDmesg, earlyBootLogging);

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

        WSLA_SESSION_SETTINGS sessionSettings = GetDefaultSessionSettings(L"termination-callback-test");
        sessionSettings.TerminationCallback = &callback;

        auto session = CreateSession(sessionSettings);

        session.reset();
        auto future = promise.get_future();
        auto result = future.wait_for(std::chrono::seconds(30));
        VERIFY_ARE_EQUAL(result, std::future_status::ready);
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, WSLAVirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

        WSLAProcessLauncher launcher("/bin/sh", {"/bin/sh"}, {"TERM=xterm-256color"}, WSLAProcessFlagsTty | WSLAProcessFlagsStdin);
        auto process = launcher.Launch(*m_defaultSession);

        wil::unique_handle tty = process.GetStdHandle(WSLAFDTty);

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(tty.get(), buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(tty.get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("\033[?2004hsh-5.2# ");
        writeTty("echo OK\n");
        validateTtyOutput("echo OK\r\n\033[?2004l\rOK");

        // Exit the shell
        writeTty("exit\n");

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 1000));
    }

    void ValidateNetworking(WSLANetworkingMode mode, bool enableDnsTunneling = false)
    {
        WSL2_TEST_ONLY();

        // Reuse the default session if settings match (same networking mode and DNS tunneling setting).
        auto createNewSession = mode != m_defaultSessionSettings.NetworkingMode ||
                                enableDnsTunneling != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsDnsTunneling);

        auto settings = GetDefaultSessionSettings(L"networking-test", false, mode);
        WI_UpdateFlag(settings.FeatureFlags, WslaFeatureFlagsDnsTunneling, enableDnsTunneling);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);

        // Verify that /etc/resolv.conf is correctly configured.
        if (enableDnsTunneling)
        {
            auto result = ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver ", "/etc/resolv.conf"}, 0);

            VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
        }
    }

    TEST_METHOD(NATNetworking)
    {
        ValidateNetworking(WSLANetworkingModeNAT);
    }

    TEST_METHOD(NATNetworkingWithDnsTunneling)
    {
        ValidateNetworking(WSLANetworkingModeNAT, true);
    }

    TEST_METHOD(VirtioProxyNetworking)
    {
        ValidateNetworking(WSLANetworkingModeVirtioProxy);
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

    void ValidatePortMapping(WSLANetworkingMode networkingMode)
    {
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings(L"port-mapping-test");
        settings.NetworkingMode = networkingMode;

        // Reuse the default session if the networking mode matches.
        auto createNewSession = networkingMode != m_defaultSessionSettings.NetworkingMode;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Install socat in the container.
        //
        // TODO: revisit this in the future to avoid pulling packages from the network.
        auto installSocat = WSLAProcessLauncher("/bin/sh", {"/bin/sh", "-c", "tdnf install socat -y"}).Launch(*session);
        ValidateProcessOutput(installSocat, {}, 0, 300 * 1000);

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

    TEST_METHOD(PortMappingNat)
    {
        ValidatePortMapping(WSLANetworkingModeNAT);
    }

    TEST_METHOD(PortMappingVirtioProxy)
    {
        ValidatePortMapping(WSLANetworkingModeVirtioProxy);
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

        // Create a 'stuck' process
        auto process = WSLAProcessLauncher{"/bin/cat", {"/bin/cat"}, {}, WSLAProcessFlagsStdin}.Launch(*m_defaultSession);

        // Stop the service
        StopWslaService();

        ResetTestSession(); // Reopen the session since the service was stopped.
    }

    void ValidateWindowsMounts(bool enableVirtioFs)
    {
        auto settings = GetDefaultSessionSettings(L"windows-mount-tests");
        WI_UpdateFlag(settings.FeatureFlags, WslaFeatureFlagsVirtioFs, enableVirtioFs);

        // Reuse the default session if possible.
        auto createNewSession = enableVirtioFs != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

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

        auto result = ExpectCommandResult(
            m_defaultSession.get(), {"/bin/sh", "-c", "echo /proc/self/fd/* && (readlink -v /proc/self/fd/* || true)"}, 0);

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

        // Validate that trying to mount the shares without GPU support enabled fails.
        {
            auto settings = GetDefaultSessionSettings(L"gpu-test-disabled");
            WI_ClearFlag(settings.FeatureFlags, WslaFeatureFlagsGPU);

            auto createNewSession = WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsGPU);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

            // Validate that the GPU device is not available.
            ExpectMount(session.get(), "/usr/lib/wsl/drivers", {});
            ExpectMount(session.get(), "/usr/lib/wsl/lib", {});
        }

        // Validate that the GPU device is available when enabled.
        {
            auto settings = GetDefaultSessionSettings(L"gpu-test");
            WI_SetFlag(settings.FeatureFlags, WslaFeatureFlagsGPU);

            auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsGPU);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

            // Validate that the GPU device is available.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -c /dev/dxg"}, 0);

            ExpectMount(
                session.get(),
                "/usr/lib/wsl/drivers",
                "/usr/lib/wsl/drivers*9p*relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            ExpectMount(
                session.get(),
                "/usr/lib/wsl/lib",
                "/usr/lib/wsl/lib none*overlay ro,relatime,lowerdir=/usr/lib/wsl/lib/packaged*");

            // Validate that the mount points are not writeable.
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}).Code, 1L);
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}).Code, 1L);
        }
    }

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        // Sanity check.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    TEST_METHOD(PmemVhds)
    {
        WSL2_TEST_ONLY();

        // Test with SCSI boot VHDs.
        {
            auto settings = GetDefaultSessionSettings(L"pmem-vhd-test");
            WI_ClearFlag(settings.FeatureFlags, WslaFeatureFlagsPmemVhds);

            auto createNewSession = WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsPmemVhds);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

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
            auto settings = GetDefaultSessionSettings(L"pmem-vhd-test");
            WI_SetFlag(settings.FeatureFlags, WslaFeatureFlagsPmemVhds);

            auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsPmemVhds);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

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

        // Simple case
        {
            auto result = ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "echo OK"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Output[2], "");
        }

        // Stdout + stderr
        {

            auto result = ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "echo stdout && (echo stderr 1>& 2)"}, 0);
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

            WSLAProcessLauncher launcher("/bin/sh", {"/bin/sh", "-c", "cat && (echo completed 1>& 2)"}, {}, WSLAProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(process.GetStdHandle(0), largeBuffer));
            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_IS_TRUE(std::equal(largeBuffer.begin(), largeBuffer.end(), result.Output[1].begin(), result.Output[1].end()));
            VERIFY_ARE_EQUAL(result.Output[2], "completed\n");
        }

        // Create a stuck process and kill it.
        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLAProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

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

            auto [hresult, error, process] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 2); // ENOENT
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/", {});

            auto [hresult, error, process] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 13); // EACCESS
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLAProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);
            auto dummyHandle = process.GetStdHandle(1);

            // Verify that the same handle can only be acquired once.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(1, reinterpret_cast<ULONG*>(&dummyHandle)), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that trying to acquire a std handle that doesn't exist fails as expected.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(3, reinterpret_cast<ULONG*>(&dummyHandle)), E_INVALIDARG);

            // Validate that the process object correctly handle requests after the VM has terminated.
            ResetTestSession();
            VERIFY_ARE_EQUAL(process.Get().Signal(WSLASignalSIGKILL), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLAProcessLauncher launcher({"/usr/bin/echo"}, {"/usr/bin/echo", "foo", "", "bar"});

            auto process = launcher.Launch(*m_defaultSession);
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // expect two spaces for the empty argument.
        }

        // Validate error paths
        {
            WSLAProcessLauncher launcher("/bin/bash", {"/bin/bash"});
            launcher.SetUser("nobody"); // Custom users are not supported for root namespace processes.

            auto [hresult, error, process] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }
    }

    TEST_METHOD(CrashDumpCollection)
    {
        WSL2_TEST_ONLY();

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
            WSLAProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLAProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

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

        constexpr auto formatedVhd = L"test-format-vhd.vhdx";

        // TODO: Replace this by a proper SDK method once it exists
        auto tokenInfo = wil::get_token_information<TOKEN_USER>();
        wsl::core::filesystem::CreateVhd(formatedVhd, 100 * 1024 * 1024, tokenInfo->User.Sid, false, false);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(formatedVhd)); });

        // Format the disk.
        auto absoluteVhdPath = std::filesystem::absolute(formatedVhd).wstring();
        VERIFY_SUCCEEDED(m_defaultSession->FormatVirtualDisk(absoluteVhdPath.c_str()));

        // Validate error paths.
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"DoesNotExist.vhdx"), E_INVALIDARG);
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"C:\\DoesNotExist.vhdx"), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    TEST_METHOD(CreateContainer)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        // Test a simple container start.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-simple", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});
        }

        // Validate that env is correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-env", {"/bin/sh", "-c", "echo $testenv"}, {{"testenv=testvalue"}});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that exit codes are correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-exit-code", {"/bin/sh", "-c", "exit 12"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {}, 12);
        }

        // Validate that stdin is correctly wired
        {
            WSLAContainerLauncher launcher(
                "debian:latest", "test-default-entrypoint", {"/bin/cat"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST, WSLAProcessFlagsStdin);

            auto container = launcher.Launch(*m_defaultSession);

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
            WSLAContainerLauncher launcher("debian:latest", "test-stdin", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            process.GetStdHandle(0); // Close stdin;

            ValidateProcessOutput(process, {{1, ""}});
        }

        // Validate that the default stop signal is respected.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-stop-signal-1", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            launcher.SetDefaultStopSignal(WSLASignalSIGHUP);
            launcher.SetContainerFlags(WSLAContainerFlagsInit);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalNone, 60));

            // Validate that the init process exited with the expected signal.
            VERIFY_ARE_EQUAL(process.Wait(), WSLASignalSIGHUP + 128);
        }

        // Validate that the default stop signal can be overriden.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-stop-signal-2", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            launcher.SetDefaultStopSignal(WSLASignalSIGHUP);
            launcher.SetContainerFlags(WSLAContainerFlagsInit);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 60));

            // Validate that the init process exited with the expected signal.
            VERIFY_ARE_EQUAL(process.Wait(), WSLASignalSIGKILL + 128);
        }

        // Validate that entrypoint is respected.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-entrypoint", {"OK"});
            launcher.SetEntrypoint({"/bin/echo", "-n"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK"}});
        }

        // Validate that the working directory is correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-stop-signal-1", {"pwd"});
            launcher.SetWorkingDirectory("/tmp");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "/tmp\n"}});
        }

        // Validate that hostname and domainanme are correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-hostname", {"/bin/sh", "-c", "echo $(hostname).$(domainname)"});

            launcher.SetHostname("my-host-name");
            launcher.SetDomainname("my-domain-name");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "my-host-name.my-domain-name\n"}});
        }

        // Validate that the username is correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-username", {"whoami"});

            launcher.SetUser("nobody");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "nobody\n"}});
        }

        // Validate that the group is correctly wired.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-group", {"groups"});

            launcher.SetUser("nobody:www-data");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "www-data\n"}});
        }

        // Validate error handling when the username / group doesn't exist

        {
            WSLAContainerLauncher launcher("debian:latest", "test-no-missing-user", {"groups"});

            launcher.SetUser("does-not-exist");

            auto [result, _] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(result, E_FAIL);

            ValidateCOMErrorMessage(L"The specified user does not exist.");
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-empty-args", {"echo", "foo", "", "bar"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // Expect two spaces for the empty argument.
        }

        // Validate error paths
        {
            WSLAContainerLauncher launcher("debian:latest", std::string(WSLA_MAX_CONTAINER_NAME_LENGTH + 1, 'a'), {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher(std::string(WSLA_MAX_IMAGE_NAME_LENGTH + 1, 'a'), "dummy", {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher("invalid-image-name", "dummy", {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, WSLA_E_IMAGE_NOT_FOUND);
        }

        {
            WSLAContainerLauncher launcher("debian:latest", "dummy", {"/does-not-exist"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);

            ValidateCOMErrorMessage(L"The specified executable was not found inside the container image.");
        }

        // Test null image name
        {
            WSLA_CONTAINER_OPTIONS options{};
            options.Image = nullptr;
            options.Name = "test-container";
            options.InitProcessOptions.CommandLine = {.Values = nullptr, .Count = 0};

            wil::com_ptr<IWSLAContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test null container name
        {
            WSLA_CONTAINER_OPTIONS options{};
            options.Image = "debian:latest";
            options.Name = nullptr;
            options.InitProcessOptions.CommandLine = {.Values = nullptr, .Count = 0};

            wil::com_ptr<IWSLAContainer> container;
            VERIFY_SUCCEEDED(m_defaultSession->CreateContainer(&options, &container));
            VERIFY_SUCCEEDED(container->Delete());
        }
    }

    TEST_METHOD(OpenContainer)
    {
        WSL2_TEST_ONLY();

        auto expectOpen = [&](const char* Id, HRESULT expectedResult = S_OK) {
            wil::com_ptr<IWSLAContainer> container;
            auto result = m_defaultSession->OpenContainer(Id, &container);

            VERIFY_ARE_EQUAL(result, expectedResult);

            return container;
        };

        {
            WSLAContainerLauncher launcher("debian:latest", "named-container", {"echo", "OK"});
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            VERIFY_ARE_EQUAL(container->Id().length(), WSLA_CONTAINER_ID_LENGTH);

            VERIFY_ARE_EQUAL(container->Name(), "named-container");

            // Validate that the container can be opened by name.
            expectOpen("named-container");

            // Validate that the container can be opened by ID.
            expectOpen(container->Id().c_str());

            // Validate that the container can be opened by a prefix of the ID.
            expectOpen(container->Id().substr(0, 8).c_str());
            expectOpen(container->Id().substr(0, 1).c_str());

            // Validate that prefix conflicts are correctly handled.
            std::vector<RunningWSLAContainer> createdContainers;
            createdContainers.emplace_back(std::move(container.value()));

            auto findConflict = [&]() {
                for (auto& e : createdContainers)
                {
                    auto firstChar = e.Id()[0];

                    if (std::ranges::count_if(createdContainers, [&](auto& container) { return container.Id()[0] == firstChar; }) > 1)
                    {
                        return firstChar;
                    }
                }

                return '\0';
            };

            // Create containers until we get two containers with the same first character in their ID.
            while (true)
            {
                VERIFY_IS_LESS_THAN(createdContainers.size(), 16);

                auto [result, newContainer] = WSLAContainerLauncher("debian:latest").CreateNoThrow(*m_defaultSession);
                VERIFY_SUCCEEDED(result);

                createdContainers.emplace_back(std::move(newContainer.value()));
                char conflictChar = findConflict();
                if (conflictChar == '\0')
                {
                    continue;
                }

                expectOpen(std::string{&conflictChar, 1}.c_str(), WSLA_E_CONTAINER_PREFIX_AMBIGUOUS);
                break;
            }
        }

        // Test error paths
        {
            expectOpen("", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid container name: ''");

            expectOpen("non-existing-container", HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            expectOpen("/", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid container name: '/'");

            expectOpen("?foo=bar", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid container name: '?foo=bar'");

            expectOpen("\n", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid container name: '\n'");

            expectOpen(" ", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid container name: ' '");
        }
    }

    TEST_METHOD(ContainerState)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLA_CONTAINER_STATE>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;

            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>()));
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
                WSLAContainerLauncher launcher("debian:latest", "exited-container", {"echo", "OK"});
                auto container = launcher.Launch(*m_defaultSession);
                auto process = container.GetInitProcess();

                ValidateProcessOutput(process, {{1, "OK\n"}});
                expectContainerList({{"exited-container", "debian:latest", WslaContainerStateExited}});
            }

            // Create a stuck container.
            WSLAContainerLauncher launcher("debian:latest", "test-container-1", {"sleep", "99999"});

            auto container = launcher.Launch(*m_defaultSession);

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
            VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("test-container-1", &sameContainer));

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
                "debian:latest", "test-container-2", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto container = launcher.Launch(*m_defaultSession);

            // Verify that the container is in running state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGTERM, 0));

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
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("does-not-exist", &sameContainer), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Validate that container names are unique.
        {
            WSLAContainerLauncher launcher(
                "debian:latest", "test-unique-name", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Validate that a container with the same name cannot be started
            VERIFY_ARE_EQUAL(
                WSLAContainerLauncher("debian:latest", "test-unique-name", {"echo", "OK"}).LaunchNoThrow(*m_defaultSession).first,
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
            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGTERM, 0));
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that stopped containers can be deleted.
            VERIFY_SUCCEEDED(container.Get().Delete());

            // Verify that stopping a deleted container returns ERROR_INVALID_STATE.
            VERIFY_ARE_EQUAL(container.Get().Stop(WSLASignalSIGTERM, 0), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers can't be deleted again.
            VERIFY_ARE_EQUAL(container.Get().Delete(), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers don't show up in the container list.
            expectContainerList({});

            // Verify that the same name can be reused now that the container is deleted.
            WSLAContainerLauncher otherLauncher(
                "debian:latest", "test-unique-name", {"echo", "OK"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto result = otherLauncher.Launch(*m_defaultSession).GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Validate that creating and starting a container separately behaves as expected

        {
            WSLAContainerLauncher launcher("debian:latest", "test-create", {"sleep", "99999"}, {});
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateCreated);
            VERIFY_SUCCEEDED(container->Get().Start(WSLAContainerStartFlagsNone));

            // Verify that Start() can't be called again on a running container.
            VERIFY_ARE_EQUAL(container->Get().Start(WSLAContainerStartFlagsNone), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateRunning);

            VERIFY_SUCCEEDED(container->Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateExited);

            VERIFY_SUCCEEDED(container->Get().Delete());

            WSLA_CONTAINER_STATE state{};
            VERIFY_ARE_EQUAL(container->Get().GetState(&state), RPC_E_DISCONNECTED);
        }

        // Validate that containers behave correctly if they outlive their session.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-dangling-ref", {"sleep", "99999"}, {});
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Delete the container to avoid leaving it dangling after test completion.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());

            // Terminate the session
            ResetTestSession();

            // Validate that calling into the container returns RPC_S_SERVER_UNAVAILABLE.
            WSLA_CONTAINER_STATE state = WslaContainerStateRunning;
            VERIFY_ARE_EQUAL(container.Get().GetState(&state), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));
            VERIFY_ARE_EQUAL(state, WslaContainerStateInvalid);
        }
    }

    TEST_METHOD(ContainerNetwork)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLA_CONTAINER_STATE>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLA_CONTAINER> containers;

            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>()));
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
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            auto details = container.Inspect();
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "host");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());

            expectContainerList({});
        }

        {
            WSLAContainerLauncher launcher(
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "none");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGTERM, 0));

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
                {"sleep", "99999"},
                {},
                (WSLA_CONTAINER_NETWORK_TYPE)6 // WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE
            );

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(retVal.first, E_INVALIDARG);
        }

        {
            WSLAContainerLauncher launcher(
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_BRIDGE);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);
            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "bridge");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslaContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete());

            expectContainerList({});
        }
    }

    TEST_METHOD(ContainerInspect)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        // Helper to verify port mappings.
        auto expectPorts = [&](const auto& actualPorts, const std::map<std::string, std::set<std::string>>& expectedPorts) {
            VERIFY_ARE_EQUAL(actualPorts.size(), expectedPorts.size());

            for (const auto& [expectedPort, expectedHostPorts] : expectedPorts)
            {
                auto it = actualPorts.find(expectedPort);
                if (it == actualPorts.end())
                {
                    LogError("Expected port key not found: %hs", expectedPort.c_str());
                    VERIFY_FAIL();
                }

                std::set<std::string> actualHostPorts;
                for (const auto& binding : it->second)
                {
                    VERIFY_IS_FALSE(binding.HostPort.empty());

                    // WSLA always binds to localhost.
                    VERIFY_ARE_EQUAL(binding.HostIp, "127.0.0.1");

                    auto [_, inserted] = actualHostPorts.insert(binding.HostPort);
                    if (!inserted)
                    {
                        LogError("Duplicate host port %hs found for port %hs", binding.HostPort.c_str(), expectedPort.c_str());
                        VERIFY_FAIL();
                    }
                }

                VERIFY_ARE_EQUAL(actualHostPorts, expectedHostPorts);
            }
        };

        // Helper to verify mounts.
        auto expectMounts = [&](const auto& actualMounts, const std::vector<std::tuple<std::string, std::string, bool>>& expectedMounts) {
            VERIFY_ARE_EQUAL(actualMounts.size(), expectedMounts.size());

            for (const auto& [expectedDest, expectedType, expectedReadWrite] : expectedMounts)
            {
                auto it = std::ranges::find_if(actualMounts, [&](const auto& mount) { return mount.Destination == expectedDest; });
                if (it == actualMounts.end())
                {
                    LogError("Expected mount destination not found: %hs", expectedDest.c_str());
                    VERIFY_FAIL();
                }

                VERIFY_IS_FALSE(it->Type.empty());
                VERIFY_IS_FALSE(it->Source.empty());

                VERIFY_ARE_EQUAL(it->Type, expectedType);
                VERIFY_ARE_EQUAL(it->ReadWrite, expectedReadWrite);
            }
        };

        // Test a running container with port mappings and volumes.
        {
            auto testFolder = std::filesystem::current_path() / "test-inspect-volume";
            auto testFolderReadOnly = std::filesystem::current_path() / "test-inspect-volume-ro";

            std::filesystem::create_directories(testFolder);
            std::filesystem::create_directories(testFolderReadOnly);

            auto cleanup = wil::scope_exit([&]() {
                std::error_code ec;
                std::filesystem::remove_all(testFolder, ec);
                std::filesystem::remove_all(testFolderReadOnly, ec);
            });

            WSLAContainerLauncher launcher(
                "debian:latest", "test-container-inspect", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_HOST);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1235, 8000, AF_INET);
            launcher.AddPort(1236, 8001, AF_INET);
            launcher.AddVolume(testFolder.wstring(), "/test-volume", false);
            launcher.AddVolume(testFolderReadOnly.wstring(), "/test-volume-ro", true);

            auto container = launcher.Launch(*m_defaultSession);
            auto details = container.Inspect();

            // Verify basic container metadata.
            VERIFY_IS_FALSE(details.Id.empty());
            VERIFY_ARE_EQUAL(details.Name, "test-container-inspect");
            VERIFY_ARE_EQUAL(details.Image, "debian:latest");
            VERIFY_IS_FALSE(details.Created.empty());

            // Verify container state.
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "host");
            VERIFY_IS_TRUE(details.State.Running);
            VERIFY_ARE_EQUAL(details.State.Status, "running");
            VERIFY_IS_FALSE(details.State.StartedAt.empty());

            // Verify port mappings match what we configured.
            expectPorts(details.Ports, {{"8000/tcp", {"1234", "1235"}}, {"8001/tcp", {"1236"}}});

            // Verify volume mounts match what we configured.
            expectMounts(details.Mounts, {{"/test-volume", "bind", true}, {"/test-volume-ro", "bind", false}});

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());
        }

        // Test an exited container still returns correct schema shape.
        {
            WSLAContainerLauncher launcher("debian:latest", "test-container-inspect-exited", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);

            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK\n"}});

            auto details = container.Inspect();

            // Verify basic container metadata is present.
            VERIFY_IS_FALSE(details.Id.empty());
            VERIFY_ARE_EQUAL(details.Name, "test-container-inspect-exited");
            VERIFY_ARE_EQUAL(details.Image, "debian:latest");
            VERIFY_IS_FALSE(details.Created.empty());

            // Verify exited state is correct.
            VERIFY_IS_FALSE(details.State.Running);
            VERIFY_ARE_EQUAL(details.State.Status, "exited");
            VERIFY_ARE_EQUAL(details.State.ExitCode, 0);
            VERIFY_IS_FALSE(details.State.StartedAt.empty());
            VERIFY_IS_FALSE(details.State.FinishedAt.empty());

            // Verify no ports or mounts for this simple container.
            expectPorts(details.Ports, {});
            expectMounts(details.Mounts, {});

            VERIFY_SUCCEEDED(container.Get().Delete());
        }
    }

    TEST_METHOD(Exec)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        // Create a container.
        WSLAContainerLauncher launcher(
            "debian:latest", "test-container-exec", {"sleep", "99999"}, {}, WSLA_CONTAINER_NETWORK_TYPE::WSLA_CONTAINER_NETWORK_NONE);

        auto container = launcher.Launch(*m_defaultSession);

        // Simple exec case.
        {
            auto process = WSLAProcessLauncher({}, {"echo", "OK"}).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "OK\n"}});
        }

        // Validate that the working directory is correctly wired.
        {
            WSLAProcessLauncher launcher({}, {"pwd"});
            launcher.SetWorkingDirectory("/tmp");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "/tmp\n"}});
        }

        // Validate that the username is correctly wired.
        {
            WSLAProcessLauncher launcher({}, {"whoami"});
            launcher.SetUser("nobody");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "nobody\n"}});
        }

        // Validate that the group is correctly wired.
        {
            WSLAProcessLauncher launcher({}, {"groups"});
            launcher.SetUser("nobody:www-data");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "www-data\n"}});
        }

        // Validate that stdin is correctly wired.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, WSLAProcessFlagsStdin).Launch(container.Get());

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
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, WSLAProcessFlagsStdin).Launch(container.Get());

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
            auto process = WSLAProcessLauncher({}, {"/bin/sh", "-c", "echo $testenv"}, {{"testenv=testvalue"}}).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLAProcessLauncher launcher({}, {"echo", "foo", "", "bar"});

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // Expect two spaces for the empty argument.
        }

        // Validate that an exec'd command returns when the container is stopped.
        {
            auto process = WSLAProcessLauncher({}, {"/bin/cat"}, {}, WSLAProcessFlagsStdin).Launch(container.Get());

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 128 + WSLASignalSIGKILL);
        }

        // Validate error paths
        {
            // Validate that processes can't be launched in stopped containers.
            auto [result, _, __] = WSLAProcessLauncher({}, {"/bin/cat"}).LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
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

    void RunPortMappingsTest(IWSLASession& session, WSLA_CONTAINER_NETWORK_TYPE containerNetworkType)
    {
        LogInfo("Container network type: %d", static_cast<int>(containerNetworkType));

        auto expectBoundPorts = [&](RunningWSLAContainer& Container, const std::vector<std::string>& expectedBoundPorts) {
            auto ports = Container.Inspect().Ports;

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
                "python:3.12-alpine", "test-ports", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6);

            auto container = launcher.Launch(session);
            auto initProcess = container.GetInitProcess();
            auto stdoutHandle = initProcess.GetStdHandle(1);

            // Wait for the container bind() to be completed.
            WaitForOutput(stdoutHandle.get(), "Serving HTTP on 0.0.0.0 port 8000");

            expectBoundPorts(container, {"8000/tcp"});

            ExpectHttpResponse(L"http://127.0.0.1:1234", 200);
            ExpectHttpResponse(L"http://[::1]:1234", {});

            // Validate that the port cannot be reused while the container is running.
            WSLAContainerLauncher subLauncher(
                "python:3.12-alpine", "test-ports-2", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            subLauncher.AddPort(1234, 8000, AF_INET);
            auto [hresult, newContainer] = subLauncher.LaunchNoThrow(session);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());

            container.Reset(); // TODO: Re-think container lifetime management.

            // Validate that the port can be reused now that the container is stopped.
            {
                WSLAContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-3", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

                launcher.AddPort(1234, 8000, AF_INET);

                auto container = launcher.Launch(session);
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
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET);

            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
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
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1235, 8000, AF_INET);
            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEACCES));

            // Validate that Create() correctly cleans up bound ports after a port fails to map
            {
                WSLAContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);
                launcher.AddPort(1236, 8000, AF_INET); // Should succeed
                launcher.AddPort(1235, 8000, AF_INET); // Should fail.

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEACCES));

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
                containerNetworkType,
                ProcessFlags::Stdout | ProcessFlags::Stderr);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6);

            auto container = launcher.Launch(session);
            auto initProcess = container.GetInitProcess();
            auto stdoutHandle = initProcess.GetStdHandle(1);

            // Wait for the container bind() to be completed.
            WaitForOutput(stdoutHandle.get(), "Serving HTTP on ::1 port 8000");

            ExpectHttpResponse(L"http://localhost:1234", {});

            ExpectHttpResponse(L"http://[::1]:1234", 200);
        }*/
    }

    auto SetupPortMappingsTest(WSLANetworkingMode networkingMode)
    {
        auto settings = GetDefaultSessionSettings(L"networking-session", true, networkingMode);

        auto createNewSession = settings.NetworkingMode != m_defaultSessionSettings.NetworkingMode;
        auto restore = createNewSession ? std::optional{ResetTestSession()} : std::nullopt;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        return std::make_pair(std::move(restore), std::move(session));
    }

    TEST_METHOD(PortMappingsNat)
    {
        WSL2_TEST_ONLY();

        auto [restore, session] = SetupPortMappingsTest(WSLANetworkingModeNAT);

        RunPortMappingsTest(*session, WSLA_CONTAINER_NETWORK_BRIDGE);
        RunPortMappingsTest(*session, WSLA_CONTAINER_NETWORK_HOST);
    }

    TEST_METHOD(PortMappingsVirtioProxy)
    {
        WSL2_TEST_ONLY();

        auto [restore, session] = SetupPortMappingsTest(WSLANetworkingModeVirtioProxy);

        RunPortMappingsTest(*session, WSLA_CONTAINER_NETWORK_BRIDGE);
        RunPortMappingsTest(*session, WSLA_CONTAINER_NETWORK_HOST);
    }

    TEST_METHOD(PortMappingsNone)
    {
        // Validate that trying to map ports without network fails.
        WSLAContainerLauncher launcher(
            "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, WSLA_CONTAINER_NETWORK_NONE);

        launcher.AddPort(1234, 8000, AF_INET);

        VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*m_defaultSession).first, E_INVALIDARG);
    }

    void ValidateContainerVolumes(bool enableVirtioFs)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto restore = ResetTestSession();
        auto hostFolder = std::filesystem::current_path() / "test-volume";
        auto hostFolderReadOnly = std::filesystem::current_path() / "test-volume-ro";

        std::filesystem::create_directories(hostFolder);
        std::filesystem::create_directories(hostFolderReadOnly);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(hostFolderReadOnly, ec);
        });

        auto settings = GetDefaultSessionSettings(L"volumes-tests", true);
        WI_UpdateFlag(settings.FeatureFlags, WslaFeatureFlagsVirtioFs, enableVirtioFs);

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

        WSLAContainerLauncher launcher("debian:latest", containerName, {"/bin/sh", "-c", script});
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

    void ValidateContainerVolumeUnmountAllFoldersOnError(bool enableVirtioFs)
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

        auto settings = GetDefaultSessionSettings(L"unmount-test");
        WI_UpdateFlag(settings.FeatureFlags, WslaFeatureFlagsVirtioFs, enableVirtioFs);

        // Reuse the default session if possible.
        auto createNewSession = enableVirtioFs != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslaFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Create a container with a simple command.
        WSLAContainerLauncher launcher("debian:latest", "test-container", {"/bin/echo", "OK"});
        launcher.AddVolume(hostFolder.wstring(), "/volume", false);

        // Add a volume with an invalid (non-existing) host path
        launcher.AddVolume(L"does-not-exist", "/volume-invalid", false);

        auto [result, container] = launcher.LaunchNoThrow(*session);
        VERIFY_FAILED(result);

        // Verify that the first volume was mounted before the error occurred, then unmounted after failure.
        ExpectMount(session.get(), "/mnt/wsla/test-container/volumes/0", {});
    }

    TEST_METHOD(ContainerVolumeUnmountAllFoldersOnError)
    {
        ValidateContainerVolumeUnmountAllFoldersOnError(false);
    }

    TEST_METHOD(ContainerVolumeUnmountAllFoldersOnErrorVirtioFs)
    {
        ValidateContainerVolumeUnmountAllFoldersOnError(true);
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

            io.AddHandle(std::make_unique<DockerIORelayHandle>(
                std::move(readPipe), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));
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

        auto restore = ResetTestSession(); // Required to access the storage folder.

        std::string containerName = "test-container";

        // Phase 1: Create session and container, then stop the container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Create and start a container
            WSLAContainerLauncher launcher("debian:latest", containerName.c_str(), {"/bin/echo", "OK"});

            auto container = launcher.Launch(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateRunning);

            // Stop the container so it can be recovered and deleted later
            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);
        }

        // Phase 2: Create new session from same storage, recover and delete container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            auto container = OpenContainer(session.get(), containerName);
            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateExited);
            VERIFY_SUCCEEDED(container.Get().Delete());

            // Verify container is no longer accessible
            wil::com_ptr<IWSLAContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Phase 3: Create new session from same storage, verify the container is not listed.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLAContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }
    }

    TEST_METHOD(ContainerVolumeAndPortRecoveryFromStorage)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        auto restore = ResetTestSession();

        std::string containerName = "test-recovery-volumes-ports";

        auto hostFolder = std::filesystem::current_path() / "test-recovery-volume";
        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
        });

        // Create a test file in the host folder
        std::ofstream testFile(hostFolder / "test.txt");
        testFile << "recovery-test-content";
        testFile.close();

        // Create session and container with volumes and ports (but don't start it)
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLANetworkingModeNAT));

            WSLAContainerLauncher launcher(
                "python:3.12-alpine", containerName, {"python3", "-m", "http.server", "--directory", "/volume"}, {"PYTHONUNBUFFERED=1"}, WSLA_CONTAINER_NETWORK_BRIDGE);

            launcher.AddPort(1250, 8000, AF_INET);
            launcher.AddVolume(hostFolder.wstring(), "/volume", false);

            // Create container but don't start it
            auto container = launcher.Create(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateCreated);
        }

        // Recover the container in a new session, start it and verify volume and port mapping works.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLANetworkingModeNAT));
            auto container = OpenContainer(session.get(), containerName);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslaContainerStateCreated);
            VERIFY_SUCCEEDED(container.Get().Start(WSLAContainerStartFlagsAttach));

            auto initProcess = container.GetInitProcess();
            auto stdoutHandle = initProcess.GetStdHandle(1);
            WaitForOutput(stdoutHandle.get(), "Serving HTTP on 0.0.0.0 port 8000");

            // A 200 response also indicates the test file is available so volume was mounted correctly.
            ExpectHttpResponse(L"http://127.0.0.1:1250/test.txt", 200);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLASignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete());
        }

        // Delete the host folder to simulate volume folder being missing on recovery
        cleanup.reset();

        // Create a new session - this should succeed even though the volume folder is gone
        auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLANetworkingModeNAT));

        wil::com_ptr<IWSLAContainer> container;
        auto hr = session->OpenContainer(containerName.c_str(), &container);

        VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    TEST_METHOD(ContainerRecoveryFromStorageInvalidMetadata)
    {
        auto restore = ResetTestSession();

        {
            auto session = CreateSession(GetDefaultSessionSettings(L"persistence-invalid-metadata", true));

            // Create a docker container that has no metadata.
            auto result = RunCommand(
                session.get(), {"/usr/bin/docker", "container", "create", "--name", "test-invalid-metadata", "debian:latest"});
            VERIFY_ARE_EQUAL(result.Code, 0L);
        }

        {
            auto session = CreateSession(GetDefaultSessionSettings(L"persistence-invalid-metadata", true));

            // Try to open the container - this should fail due to missing metadata.
            wil::com_ptr<IWSLAContainer> container;
            auto hr = session->OpenContainer("test-invalid-metadata", &container);
            VERIFY_ARE_EQUAL(hr, E_UNEXPECTED);
        }
    }

    TEST_METHOD(SessionManagement)
    {
        WSL2_TEST_ONLY();

        auto manager = OpenSessionManager();

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
            return CreateSession(GetDefaultSessionSettings(Name), Flags);
        };

        // Validate that non-persistent sessions are dropped when released
        {
            auto session1 = create(L"session-1", WSLASessionFlagsNone);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that persistent sessions are only dropped when explicitly terminated.
        {
            auto session1 = create(L"session-1", WSLASessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});
            session1 = create(L"session-1", WSLASessionFlagsOpenExisting);

            VERIFY_SUCCEEDED(session1->Terminate());
            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that sessions can be reopened by name.
        {
            auto session1 = create(L"session-1", WSLASessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});

            auto session1Copy =
                create(L"session-1", static_cast<WSLASessionFlags>(WSLASessionFlagsPersistent | WSLASessionFlagsOpenExisting));

            expectSessions({L"session-1", c_testSessionName});

            // Verify that name conflicts are correctly handled.
            auto settings = GetDefaultSessionSettings(L"session-1");

            wil::com_ptr<IWSLASession> session;
            VERIFY_ARE_EQUAL(manager->CreateSession(&settings, WSLASessionFlagsPersistent, &session), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(session1Copy->Terminate());
            WSLASessionState state{};
            VERIFY_SUCCEEDED(session1Copy->GetState(&state));
            VERIFY_ARE_EQUAL(state, WSLASessionStateTerminated);
            expectSessions({c_testSessionName});

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

    static void ValidateHandleOutput(HANDLE handle, const std::string& expectedOutput)
    {
        VERIFY_ARE_EQUAL(EscapeString(expectedOutput), EscapeString(ReadToString(handle)));
    }

    TEST_METHOD(ContainerLogs)
    {
        WSL2_TEST_ONLY();

        auto expectLogs = [](auto& container,
                             const std::string& expectedStdout,
                             const std::optional<std::string>& expectedStderr,
                             WSLALogsFlags Flags = WSLALogsFlagsNone,
                             ULONGLONG Tail = 0,
                             ULONGLONG Since = 0,
                             ULONGLONG Until = 0) {
            wil::unique_handle stdoutLogs;
            wil::unique_handle stderrLogs;
            VERIFY_SUCCEEDED(container.Logs(Flags, (ULONG*)&stdoutLogs, (ULONG*)&stderrLogs, Since, Until, Tail));

            ValidateHandleOutput(stdoutLogs.get(), expectedStdout);

            if (expectedStderr.has_value())
            {
                ValidateHandleOutput(stderrLogs.get(), expectedStderr.value());
            }
        };

        // Test a simple scenario.
        {
            // Create a container with a simple command.
            WSLAContainerLauncher launcher(
                "debian:latest", "logs-test-1", {"/bin/bash", "-c", "echo stdout && (echo stderr >& 2)"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "stdout\n"}, {2, "stderr\n"}});

            expectLogs(container.Get(), "stdout\n", "stderr\n");

            // validate that logs can be queried multiple times.
            expectLogs(container.Get(), "stdout\n", "stderr\n");
        }

        // Validate that tail works.
        {
            // Create a container with a simple command.
            WSLAContainerLauncher launcher(
                "debian:latest", "logs-test-2", {"/bin/bash", "-c", "echo -en 'line1\\nline2\\nline3\\nline4'"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "line1\nline2\nline3\nline4"}});

            expectLogs(container.Get(), "line1\nline2\nline3\nline4", "");
            expectLogs(container.Get(), "line4", "", WSLALogsFlagsNone, 1);
            expectLogs(container.Get(), "line3\nline4", "", WSLALogsFlagsNone, 2);
            expectLogs(container.Get(), "line1\nline2\nline3\nline4", "", WSLALogsFlagsNone, 4);
        }

        // Validate that timestamps are correctly returned.
        {
            WSLAContainerLauncher launcher("debian:latest", "logs-test-3", {"/bin/bash", "-c", "echo -n OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            wil::unique_handle stdoutLogs;
            wil::unique_handle stderrLogs;
            VERIFY_SUCCEEDED(container.Get().Logs(WSLALogsFlagsTimestamps, (ULONG*)&stdoutLogs, (ULONG*)&stderrLogs, 0, 0, 0));

            auto output = ReadToString(stdoutLogs.get());
            VerifyPatternMatch(output, "20*-*-* OK"); // Timestamp is in ISO 8601 format
        }

        // Validate that 'since' and 'until' work as expected.
        {
            WSLAContainerLauncher launcher("debian:latest", "logs-test-4", {"/bin/bash", "-c", "echo -n OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            // Testing would with more granularity would be difficult, but these flags are just forwarded to docker,
            // so validate that they're wired correctly.

            auto now = time(nullptr);
            expectLogs(container.Get(), "OK", "", WSLALogsFlagsNone, 0, now - 3600);
            expectLogs(container.Get(), "", "", WSLALogsFlagsNone, 0, now + 3600);

            expectLogs(container.Get(), "", "", WSLALogsFlagsNone, 0, 0, now - 3600);
            expectLogs(container.Get(), "OK", "", WSLALogsFlagsNone, 0, 0, now + 3600);
        }

        // Validate that logs work for TTY processes
        {
            WSLAContainerLauncher launcher(
                "debian:latest", "logs-test-5", {"/bin/bash", "-c", "stat -f /dev/stdin | grep -io 'Type:.*$'"}, {}, {}, WSLAProcessFlagsStdin | WSLAProcessFlagsTty);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            ValidateHandleOutput(initProcess.GetStdHandle(WSLAFDTty).get(), "Type: devpts\r\n");
            VERIFY_ARE_EQUAL(initProcess.Wait(), 0);

            expectLogs(container.Get(), "Type: devpts\r\n", {});

            // Validate that logs can queried multiple times.
            expectLogs(container.Get(), "Type: devpts\r\n", {});
        }

        // Validate that the 'follow' flag works as expected.
        {
            WSLAContainerLauncher launcher("debian:latest", "logs-test-6", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            // Without 'follow', logs return immediately.
            expectLogs(container.Get(), "", "");

            // Create a 'follow' logs call.
            wil::unique_handle stdoutLogs;
            wil::unique_handle stderrLogs;
            VERIFY_SUCCEEDED(container.Get().Logs(WSLALogsFlagsFollow, (ULONG*)&stdoutLogs, (ULONG*)&stderrLogs, 0, 0, 0));

            PartialHandleRead reader(stdoutLogs.get());

            auto containerStdin = initProcess.GetStdHandle(0);
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.get(), "line1\n", 6, nullptr, nullptr));

            reader.Expect("line1\n");
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.get(), "line2\n", 6, nullptr, nullptr));
            reader.Expect("line1\nline2\n");

            containerStdin.reset();
            reader.ExpectClosed();

            expectLogs(container.Get(), "line1\nline2\n", "");
            expectLogs(container.Get(), "line1\nline2\n", "", WSLALogsFlagsFollow);
        }
    }

    TEST_METHOD(ContainerLabels)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        // Docker labels do not have a size limit, so test with a very large label value to validate that the API can handle it.
        std::map<std::string, std::string> labels = {{"key1", "value1"}, {"key2", std::string(10000, 'a')}};

        // Test valid labels
        {
            WSLAContainerLauncher launcher("debian:latest", "test-labels", {"echo", "OK"});

            for (const auto& [key, value] : labels)
            {
                launcher.AddLabel(key, value);
            }

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(labels, container.Labels());

            // Keep the container alive after the handle is dropped so we can validate labels are persisted across sessions.
            container.SetDeleteOnClose(false);
        }

        {
            // Restarting the test session will force the container to be reloaded from storage.
            ResetTestSession();

            // Validate that labels are correctly loaded.
            auto container = OpenContainer(m_defaultSession.get(), "test-labels");
            VERIFY_ARE_EQUAL(labels, container.Labels());
        }

        // Test nullptr key
        {
            WSLA_LABEL label{.Key = nullptr, .Value = "value"};

            WSLA_CONTAINER_OPTIONS options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-nullptr-key";
            options.Labels = &label;
            options.LabelsCount = 1;

            wil::com_ptr<IWSLAContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test nullptr value
        {
            WSLA_LABEL label{.Key = "key", .Value = nullptr};

            WSLA_CONTAINER_OPTIONS options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-nullptr-value";
            options.Labels = &label;
            options.LabelsCount = 1;

            wil::com_ptr<IWSLAContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test duplicate keys
        {
            std::vector<WSLA_LABEL> labels(2);
            labels.push_back({.Key = "key", .Value = "value"});
            labels.push_back({.Key = "key", .Value = "value2"});

            WSLA_CONTAINER_OPTIONS options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-duplicate-keys";
            options.Labels = labels.data();
            options.LabelsCount = static_cast<ULONG>(labels.size());

            wil::com_ptr<IWSLAContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test wsla metadata key conflict
        {
            WSLAContainerLauncher launcher("debian:latest");
            launcher.AddLabel("com.microsoft.wsl.container.metadata", "value");

            auto [hr, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }
    }

    TEST_METHOD(ContainerAttach)
    {
        WSL2_TEST_ONLY();

        // Validate attach behavior in a non-tty process.
        {
            WSLAContainerLauncher launcher("debian:latest", "attach-test-1", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            // Verify that attaching to a created container fails.
            wil::unique_handle attachedStdin;
            wil::unique_handle attachedStdout;
            wil::unique_handle attachedStderr;
            VERIFY_ARE_EQUAL(
                container->Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr),
                HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Start the container.
            VERIFY_SUCCEEDED(container->Get().Start(WSLAContainerStartFlagsAttach));

            // Get its original std handles.
            auto process = container->GetInitProcess();
            auto originalStdin = process.GetStdHandle(0);
            auto originalStdout = process.GetStdHandle(1);

            // Attach to the container with separate handles.
            VERIFY_SUCCEEDED(container->Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr));

            PartialHandleRead originalReader(originalStdout.get());
            PartialHandleRead attachedReader(attachedStdout.get());

            // Write content on the original stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(originalStdin.get(), "line1\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\n");
            attachedReader.Expect("line1\n");

            // Write content on the attached stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(attachedStdin.get(), "line2\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\nline2\n");
            attachedReader.Expect("line1\nline2\n");

            // Close the original stdin.
            originalStdin.reset();

            // Expect both readers to be closed.
            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();

            process.Wait();

            attachedStdin.reset();
            attachedStdout.reset();
            attachedStderr.reset();

            // Validate that attaching to an exited container fails.
            VERIFY_ARE_EQUAL(container->State(), WslaContainerStateExited);
            VERIFY_ARE_EQUAL(
                container->Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr),
                HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Validate that attaching to a deleted container fails.
            VERIFY_SUCCEEDED(container->Get().Delete());
            VERIFY_ARE_EQUAL(container->Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr), RPC_E_DISCONNECTED);

            container->SetDeleteOnClose(false);
        }

        // Validate that closing an attached stdin terminates the container.
        {
            WSLAContainerLauncher launcher("debian:latest", "attach-test-2", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);

            auto process = container.GetInitProcess();
            auto originalStdin = process.GetStdHandle(0);
            auto originalStdout = process.GetStdHandle(1);

            wil::unique_handle attachedStdin;
            wil::unique_handle attachedStdout;
            wil::unique_handle attachedStderr;
            VERIFY_SUCCEEDED(container.Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr));

            PartialHandleRead originalReader(originalStdout.get());
            PartialHandleRead attachedReader(attachedStdout.get());

            attachedStdin.reset();

            // Expect both readers to be closed.
            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();
        }

        // Validate behavior for tty containers
        {
            WSLAContainerLauncher launcher("debian:latest", "attach-test-3", {"/bin/bash"}, {}, {}, WSLAProcessFlagsTty | WSLAProcessFlagsStdin);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            auto originalTty = process.GetStdHandle(WSLAFDTty);

            wil::unique_handle attachedTty;
            wil::unique_handle dummy;
            VERIFY_SUCCEEDED(container.Get().Attach((ULONG*)&attachedTty, (ULONG*)&dummy, (ULONG*)&dummy));

            PartialHandleRead originalReader(originalTty.get());
            PartialHandleRead attachedReader(attachedTty.get());

            // Read the prompt from the original tty (hardcoded bytes since behavior is constant).
            auto prompt = originalReader.ReadBytes(13);
            VerifyPatternMatch(prompt, "*root@*");

            // Resize the tty to force the prompt to redraw.
            process.Get().ResizeTty(61, 81);

            auto attachedPrompt = attachedReader.ReadBytes(13);
            VerifyPatternMatch(attachedPrompt, "*root@*");

            // Close the tty.
            originalTty.reset();
            attachedTty.reset();

            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();
        }

        // Validate that containers can be started in detached mode and attached to later.
        {
            WSLAContainerLauncher launcher("debian:latest", "attach-test-4", {"/bin/cat"}, {}, {}, WSLAProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession, WSLAContainerStartFlagsNone);

            auto initProcess = container.GetInitProcess();
            ULONG dummy{};
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLAFDStdin, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLAFDStdout, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLAFDStderr, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));

            // Verify that the container can be attached to.
            wil::unique_handle attachedStdin;
            wil::unique_handle attachedStdout;
            wil::unique_handle attachedStderr;
            VERIFY_SUCCEEDED(container.Get().Attach((ULONG*)&attachedStdin, (ULONG*)&attachedStdout, (ULONG*)&attachedStderr));

            PartialHandleRead attachedReader(attachedStdout.get());

            // Write content on the attached stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(attachedStdin.get(), "OK\n", 3, nullptr, nullptr));
            attachedStdin.reset();

            attachedReader.Expect("OK\n");
            attachedReader.ExpectClosed();
            VERIFY_ARE_EQUAL(initProcess.Wait(), 0);
        }
    }

    TEST_METHOD(InvalidNames)
    {
        WSL2_TEST_ONLY();

        auto expectInvalidArg = [&](const std::string& name) {
            wil::com_ptr<IWSLAContainer> container;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(name.c_str(), &container), E_INVALIDARG);
            VERIFY_IS_NULL(container.get());

            ValidateCOMErrorMessage(std::format(L"Invalid container name: '{}'", name));
        };

        expectInvalidArg("container with spaces");
        expectInvalidArg("?foo");
        expectInvalidArg("?foo&bar");
        expectInvalidArg("/url/path");
        expectInvalidArg("");
        expectInvalidArg("\\escaped\n\\chars");

        std::string longName(WSLA_MAX_CONTAINER_NAME_LENGTH + 1, 'a');
        expectInvalidArg(longName);

        auto expectInvalidPull = [&](const char* name, const char* errorPattern) {
            VERIFY_ARE_EQUAL(m_defaultSession->PullImage(name, nullptr, nullptr), E_INVALIDARG);

            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            VerifyPatternMatch(wsl::shared::string::WideToMultiByte(comError->Message.get()), errorPattern);
        };

        expectInvalidPull("?foo&bar/url\n:name", "invalid reference format");
        expectInvalidPull("?:&", "invalid reference format");
        expectInvalidPull("/:/", "invalid reference format");
        expectInvalidPull("\n: ", "invalid reference format");
        expectInvalidPull("invalid\nrepo:valid-image", "invalid reference format");
        expectInvalidPull("bad!repo:valid-image", "invalid reference format");
        expectInvalidPull("repo:badimage!name", "invalid tag format");
        expectInvalidPull("bad+image", "invalid reference format");
    }

    TEST_METHOD(PageReporting)
    {
        WSL2_TEST_ONLY();

        // Determine expected page reporting order based on Windows version.
        // On Germanium or later: 5 (128k), otherwise: 9 (2MB).
        const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
        int expectedOrder = (windowsVersion.BuildNumber >= wsl::windows::common::helpers::WindowsBuildNumbers::Germanium) ? 5 : 9;

        // Read the actual value from sysfs and verify it matches.
        auto result =
            ExpectCommandResult(m_defaultSession.get(), {"/bin/cat", "/sys/module/page_reporting/parameters/page_reporting_order"}, 0);

        VERIFY_ARE_EQUAL(result.Output[1], std::format("{}\n", expectedOrder));
    }
};
