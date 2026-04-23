/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTests.cpp

Abstract:

    This file contains test cases for the WSLC API.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslc.h"
#include "WSLCProcessLauncher.h"
#include "WSLCContainerLauncher.h"
#include "WslCoreFilesystem.h"

using namespace std::literals::chrono_literals;
using namespace wsl::windows::common::registry;
using wsl::windows::common::RunningWSLCContainer;
using wsl::windows::common::RunningWSLCProcess;
using wsl::windows::common::WSLCContainerLauncher;
using wsl::windows::common::WSLCProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;
using namespace wsl::windows::common::wslutil;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

class WSLCTests
{
    WSLC_TEST_CLASS(WSLCTests)

    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WSLCSessionSettings m_defaultSessionSettings{};
    wil::com_ptr<IWSLCSession> m_defaultSession;
    static inline auto c_testSessionName = L"wslc-test";

    void LoadTestImage(std::string_view imageName, IWSLCSession* session = nullptr)
    {
        std::filesystem::path imagePath = GetTestImagePath(imageName);
        wil::unique_hfile imageFile{
            CreateFileW(imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!imageFile);

        LARGE_INTEGER fileSize{};
        THROW_LAST_ERROR_IF(!GetFileSizeEx(imageFile.get(), &fileSize));

        THROW_IF_FAILED(
            (session ? session : m_defaultSession.get())->LoadImage(ToCOMInputHandle(imageFile.get()), nullptr, fileSize.QuadPart));
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // The WSLC SDK tests use this same storage to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";
        m_defaultSessionSettings = GetDefaultSessionSettings(c_testSessionName, true, WSLCNetworkingModeVirtioProxy);
        m_defaultSession = CreateSession(m_defaultSessionSettings);

        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, &images, images.size_address<ULONG>()));

        auto hasImage = [&](const std::string& imageName) {
            return std::ranges::any_of(
                images.get(), images.get() + images.size(), [&](const auto& e) { return e.Image == imageName; });
        };

        if (!hasImage("debian:latest"))
        {
            LoadTestImage("debian:latest");
        }

        if (!hasImage("python:3.12-alpine"))
        {
            LoadTestImage("python:3.12-alpine");
        }

        if (!hasImage("hello-world:latest"))
        {
            LoadTestImage("hello-world:latest");
        }

        if (!hasImage("alpine:latest"))
        {
            LoadTestImage("alpine:latest");
        }

        if (!hasImage("wslc-registry:latest"))
        {
            LoadTestImage("wslc-registry:latest");
        }

        PruneResult result;
        VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(nullptr, 0, 0, &result.result));
        if (result.result.ContainersCount > 0)
        {
            LogInfo("Pruned %lu containers", result.result.ContainersCount);
        }

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

    WSLCSessionSettings GetDefaultSessionSettings(LPCWSTR Name, bool enableStorage = false, WSLCNetworkingMode networkingMode = WSLCNetworkingModeNone)
    {
        WSLCSessionSettings settings{};
        settings.DisplayName = Name;
        settings.CpuCount = 4;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = enableStorage ? m_storagePath.c_str() : nullptr;
        settings.MaximumStorageSizeMb = 1024 * 20; // 20GB.
        settings.NetworkingMode = networkingMode;

        return settings;
    }

    auto ResetTestSession()
    {
        m_defaultSession.reset();

        return wil::scope_exit([this]() { m_defaultSession = CreateSession(m_defaultSessionSettings); });
    }

    static wil::com_ptr<IWSLCSessionManager> OpenSessionManager()
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        return sessionManager;
    }

    wil::com_ptr<IWSLCSession> CreateSession(const WSLCSessionSettings& sessionSettings, WSLCSessionFlags Flags = WSLCSessionFlagsNone)
    {
        const auto sessionManager = OpenSessionManager();

        wil::com_ptr<IWSLCSession> session;

        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        WSLCSessionState state{};
        VERIFY_SUCCEEDED(session->GetState(&state));
        VERIFY_ARE_EQUAL(state, WSLCSessionStateRunning);

        return session;
    }

    RunningWSLCContainer OpenContainer(IWSLCSession* session, const std::string& name)
    {
        wil::com_ptr<IWSLCContainer> rawContainer;
        VERIFY_SUCCEEDED(session->OpenContainer(name.c_str(), &rawContainer));

        return RunningWSLCContainer(std::move(rawContainer), {});
    }

    std::pair<RunningWSLCContainer, std::string> StartLocalRegistry(const std::string& username = {}, const std::string& password = {}, USHORT port = 5000)
    {
        std::vector<std::string> env = {std::format("REGISTRY_HTTP_ADDR=0.0.0.0:{}", port)};
        if (!username.empty())
        {
            env.push_back(std::format("USERNAME={}", username));
            env.push_back(std::format("PASSWORD={}", password));
        }

        WSLCContainerLauncher launcher("wslc-registry:latest", {}, {}, env);
        launcher.SetEntrypoint({"/entrypoint.sh"});
        launcher.AddPort(port, port, AF_INET);

        auto container = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

        auto registryAddress = std::format("127.0.0.1:{}", port);
        auto registryUrl = std::format(L"http://{}", registryAddress);
        ExpectHttpResponse(registryUrl.c_str(), 200, true);

        return {std::move(container), std::move(registryAddress)};
    }

    void ExpectImagePresent(IWSLCSession& Session, const char* Image, bool Present = true)
    {
        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        THROW_IF_FAILED(Session.ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

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

    WSLC_TEST_METHOD(LoadImage)
    {
        std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "hello-world:latest");

        // Validate container launch from the loaded image
        {
            WSLCContainerLauncher launcher("hello-world:latest", "wslc-load-image-container");

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Validate that invalid tars fail with proper error message and code.
        {
            auto currentExecutableHandle = wil::open_file(wil::GetModuleFileNameW<std::wstring>().c_str());
            VERIFY_IS_TRUE(GetFileSizeEx(currentExecutableHandle.get(), &fileSize));

            VERIFY_ARE_EQUAL(m_defaultSession->LoadImage(ToCOMInputHandle(currentExecutableHandle.get()), nullptr, fileSize.QuadPart), E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }

        // Validate that LoadImage fails when the input pipe is closed during reading.
        {
            wil::unique_handle pipeRead;
            wil::unique_handle pipeWrite;
            VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

            std::promise<HRESULT> loadResult;
            std::thread operationThread([&]() {
                loadResult.set_value(m_defaultSession->LoadImage(ToCOMInputHandle(pipeRead.get()), nullptr, 1024 * 1024));
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to ensure the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            // Close the write end.
            pipeWrite.reset();

            VERIFY_ARE_EQUAL(E_FAIL, loadResult.get_future().get());
        }

        // Validate that LoadImage is aborted when the session terminates.
        // N.B. The read pipe must support overlapped IO so the relay's event-based cancellation works.
        // CreatePipe creates synchronous pipes where ReadFile blocks the thread, preventing
        // WaitForMultipleObjects from detecting the session terminating event.
        {
            auto [pipeRead, pipeWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(2, true, false);

            std::promise<HRESULT> terminateResult;
            wil::unique_event testCompleted{wil::EventOptions::ManualReset};
            std::thread operationThread([&]() {
                terminateResult.set_value(m_defaultSession->LoadImage(ToCOMInputHandle(pipeRead.get()), nullptr, 1024 * 1024));
                WI_ASSERT(testCompleted.is_signaled());
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to validate that the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            testCompleted.SetEvent();

            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto restore = ResetTestSession();

            auto hr = terminateResult.get_future().get();
            VERIFY_IS_TRUE(hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
        }
    }

    void ValidateCOMErrorMessage(const std::optional<std::wstring>& Expected, const std::source_location& Source = std::source_location::current())
    {
        auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();

        if (comError.has_value())
        {
            if (!Expected.has_value())
            {
                LogError("Unexpected COM error: '%ls'. Source: %hs", comError->Message.get(), std::format("{}", Source).c_str());
                VERIFY_FAIL();
            }

            VERIFY_ARE_EQUAL(Expected.value(), comError->Message.get());
        }
        else
        {
            if (Expected.has_value())
            {
                LogError("Expected COM error: '%ls' but none was set. Source: %hs", Expected->c_str(), std::format("{}", Source).c_str());
                VERIFY_FAIL();
            }
        }
    }
};
