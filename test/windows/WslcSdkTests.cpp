/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSdkTests.cpp

Abstract:

    This file contains test cases for the WSLC SDK.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslcsdk.h"
#include "wslc_schema.h"
#include <optional>

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

using namespace std::chrono_literals;

namespace {

//
// RAII guards for opaque WSLC handle types.
//

void CloseSession(WslcSession session)
{
    if (session)
    {
        WslcTerminateSession(session);
        WslcReleaseSession(session);
    }
}

using UniqueSession = wil::unique_any<WslcSession, decltype(CloseSession), CloseSession>;

void CloseContainer(WslcContainer container)
{
    if (container)
    {
        WslcStopContainer(container, WSLC_SIGNAL_SIGKILL, 0, nullptr);
        WslcDeleteContainer(container, WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr);
        WslcReleaseContainer(container);
    }
}

using UniqueContainer = wil::unique_any<WslcContainer, decltype(CloseContainer), CloseContainer>;

void CloseProcess(WslcProcess process)
{
    if (process)
    {
        WslcReleaseProcess(process);
    }
}

using UniqueProcess = wil::unique_any<WslcProcess, decltype(CloseProcess), CloseProcess>;

struct ProcessOutput
{
    std::string stdoutOutput;
    std::string stderrOutput;
};

ProcessOutput WaitForProcessOutput(WslcProcess process, std::chrono::milliseconds timeout = 2min)
{
    // Borrow the exit-event handle (lifetime tied to the process object; do NOT close it).
    HANDLE exitEvent = nullptr;
    THROW_IF_FAILED(WslcGetProcessExitEvent(process, &exitEvent));

    // Acquire stdout / stderr pipe handles (caller owns these).
    wil::unique_handle ownedStdout;
    THROW_IF_FAILED(WslcGetProcessIOHandle(process, WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

    wil::unique_handle ownedStderr;
    THROW_IF_FAILED(WslcGetProcessIOHandle(process, WSLC_PROCESS_IO_HANDLE_STDERR, &ownedStderr));

    // Read stdout / stderr concurrently so that full pipe buffers do not stall the process.
    ProcessOutput output;
    wsl::windows::common::relay::MultiHandleWait io;

    io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
        std::move(ownedStdout), [&](const auto& buffer) { output.stdoutOutput.append(buffer.data(), buffer.size()); }));

    io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
        std::move(ownedStderr), [&](const auto& buffer) { output.stderrOutput.append(buffer.data(), buffer.size()); }));

    auto timeoutTime = std::chrono::steady_clock::now() + timeout;
    io.Run(timeout);

    auto remaining = timeoutTime - std::chrono::steady_clock::now();
    if (remaining < 0ns)
    {
        remaining = {};
    }

    // Check that the process exits within the timeout.
    THROW_HR_IF(
        HRESULT_FROM_WIN32(WAIT_TIMEOUT),
        WaitForSingleObject(exitEvent, static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count())) != WAIT_OBJECT_0);

    return output;
}

//
// Runs a container with the given argv, waits up to timeoutMs for it to exit,
// and returns the captured stdout / stderr output.
//
ProcessOutput RunContainerAndCapture(WslcSession session, const WslcContainerSettings& containerSettings, std::chrono::milliseconds timeout = 2min)
{
    // Create and start the container.
    UniqueContainer container;
    THROW_IF_FAILED(WslcCreateContainer(session, &containerSettings, &container, nullptr));
    THROW_IF_FAILED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

    // Acquire the init process handle.
    UniqueProcess process;
    THROW_IF_FAILED(WslcGetContainerInitProcess(container.get(), &process));

    return WaitForProcessOutput(process.get());
}

ProcessOutput RunContainerAndCapture(
    WslcSession session,
    const char* image,
    const std::vector<const char*>& argv,
    WslcContainerFlags flags = WSLC_CONTAINER_FLAG_NONE,
    const char* name = nullptr,
    std::chrono::milliseconds timeout = 2min,
    std::optional<WslcContainerNetworkingMode> networkingMode = std::nullopt)
{
    // Build process settings.
    WslcProcessSettings procSettings;
    THROW_IF_FAILED(WslcInitProcessSettings(&procSettings));
    if (!argv.empty())
    {
        THROW_IF_FAILED(WslcSetProcessSettingsCmdLine(&procSettings, argv.data(), argv.size()));
    }

    // Build container settings.
    WslcContainerSettings containerSettings;
    THROW_IF_FAILED(WslcInitContainerSettings(image, &containerSettings));
    THROW_IF_FAILED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
    THROW_IF_FAILED(WslcSetContainerSettingsFlags(&containerSettings, flags));
    if (name)
    {
        THROW_IF_FAILED(WslcSetContainerSettingsName(&containerSettings, name));
    }
    if (networkingMode.has_value())
    {
        THROW_IF_FAILED(WslcSetContainerSettingsNetworkingMode(&containerSettings, *networkingMode));
    }

    return RunContainerAndCapture(session, containerSettings, timeout);
}

} // namespace

class WslcSdkTests
{
    WSLC_TEST_CLASS(WslcSdkTests)

    wil::unique_mta_usage_cookie m_mtaCookie;
    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WslcSession m_defaultSession = nullptr;
    static inline auto c_testSessionName = L"wslc-test";

    void LoadTestImage(std::string_view imageName)
    {
        std::filesystem::path imagePath = GetTestImagePath(imageName);
        THROW_IF_FAILED(WslcLoadSessionImageFromFile(m_defaultSession, imagePath.c_str(), nullptr, nullptr));
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // Use the same storage path as WSLC runtime tests to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";

        // Build session settings using the WSLC SDK.
        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(c_testSessionName, m_storagePath.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsCpuCount(&sessionSettings, 4));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsMemory(&sessionSettings, 2048));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 4096ull * 1024 * 1024; // 4 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSetSessionSettingsVhd(&sessionSettings, &vhdReqs));

        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &m_defaultSession, nullptr));

        // Pull images required by the tests (no-op if already present).
        for (const char* image : {"debian:latest", "python:3.12-alpine"})
        {
            LoadTestImage(image);
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_defaultSession)
        {
            WslcTerminateSession(m_defaultSession);
            WslcReleaseSession(m_defaultSession);
            m_defaultSession = nullptr;
        }

        // Preserve the VHD in fast-run mode so subsequent runs skip image pulling.
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

    // -----------------------------------------------------------------------
    // Session tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(CreateSession)
    {
        std::filesystem::path extraStorage = m_storagePath / "wslc-extra-session-storage";

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(L"wslc-extra-session", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsCpuCount(&sessionSettings, 2));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsMemory(&sessionSettings, 1024));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 1024ull * 1024 * 1024; // 1 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSetSessionSettingsVhd(&sessionSettings, &vhdReqs));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &session, nullptr));
        VERIFY_IS_NOT_NULL(session.get());

        // Null output pointer must fail.
        VERIFY_ARE_EQUAL(WslcCreateSession(&sessionSettings, nullptr, nullptr), E_POINTER);

        // Null settings pointer must fail.
        UniqueSession session2;
        VERIFY_ARE_EQUAL(WslcCreateSession(nullptr, &session2, nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(TerminationCallbackViaTerminate)
    {
        std::promise<WslcSessionTerminationReason> promise;

        auto callback = [](WslcSessionTerminationReason reason, PVOID context) {
            auto* p = static_cast<std::promise<WslcSessionTerminationReason>*>(context);
            p->set_value(reason);
        };

        std::filesystem::path extraStorage = m_storagePath / "wslc-termcb-term-storage";

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(L"wslc-termcb-term-test", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTerminationCallback(&sessionSettings, callback, &promise));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &session, nullptr));

        // Terminating the session should trigger a graceful shutdown and fire the callback.
        VERIFY_SUCCEEDED(WslcTerminateSession(session.get()));

        auto future = promise.get_future();
        VERIFY_ARE_EQUAL(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
        VERIFY_ARE_EQUAL(future.get(), WSLC_SESSION_TERMINATION_REASON_SHUTDOWN);
    }

    WSLC_TEST_METHOD(TerminationCallbackViaRelease)
    {
        std::promise<WslcSessionTerminationReason> promise;

        auto callback = [](WslcSessionTerminationReason reason, PVOID context) {
            auto* p = static_cast<std::promise<WslcSessionTerminationReason>*>(context);
            p->set_value(reason);
        };

        std::filesystem::path extraStorage = m_storagePath / "wslc-termcb-release-storage";

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(L"wslc-termcb-release-test", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTerminationCallback(&sessionSettings, callback, &promise));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &session, nullptr));

        // Releasing the session should trigger a graceful shutdown and fire the callback.
        VERIFY_SUCCEEDED(WslcReleaseSession(session.get()));
        // Calling WslcSessionRelease will destroy the session
        session.release();

        auto future = promise.get_future();
        VERIFY_ARE_EQUAL(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
        VERIFY_ARE_EQUAL(future.get(), WSLC_SESSION_TERMINATION_REASON_SHUTDOWN);
    }

    // -----------------------------------------------------------------------
    // Image tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(PullImage)
    {
        // Positive: pull a well-known image.
        {
            WslcPullImageOptions opts{};
            opts.uri = "hello-world:linux";
            wil::unique_cotaskmem_string errorMsg;
            HRESULT pullResult = WslcPullSessionImage(m_defaultSession, &opts, &errorMsg);

            // Skip test if error is due to rate limit.
            if (pullResult == E_FAIL)
            {
                if (errorMsg)
                {
                    if (wcsstr(errorMsg.get(), L"toomanyrequests") != nullptr)
                    {
                        LogWarning("Skipping PullImage test due to rate limiting.");
                        return;
                    }
                }
            }

            VERIFY_SUCCEEDED(pullResult);

            // Verify the image is usable by running a container from it.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:linux", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Negative: pull an image that does not exist.
        {
            WslcPullImageOptions opts{};
            opts.uri = "does-not:exist";
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(WslcPullSessionImage(m_defaultSession, &opts, &errorMsg), WSLC_E_IMAGE_NOT_FOUND);

            // An error message should be present.
            VERIFY_IS_NOT_NULL(errorMsg.get());
        }

        // Negative: null options pointer must fail.
        {
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(WslcPullSessionImage(m_defaultSession, nullptr, &errorMsg), E_POINTER);
        }

        // Negative: null URI inside options must fail.
        {
            WslcPullImageOptions opts{};
            opts.uri = nullptr;
            VERIFY_ARE_EQUAL(WslcPullSessionImage(m_defaultSession, &opts, nullptr), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ImageList)
    {
        // Positive: session has images pre-loaded — list must return at least one entry.
        {
            WslcImageInfo* images = nullptr;
            uint32_t count = 0;
            VERIFY_SUCCEEDED(WslcListSessionImages(m_defaultSession, &images, &count));
            auto cleanupImages = wil::scope_exit([images]() { CoTaskMemFree(images); });
            VERIFY_IS_TRUE(count >= 1);
            VERIFY_IS_NOT_NULL(images);
            // At least one image must have a non-empty name.
            bool foundNonEmpty = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (images[i].name[0] != '\0' && (images[i].sha256[0] != 0 || images[i].sha256[31] != 0) &&
                    images[i].sizeBytes != 0 && images[i].createdTimestamp != 0)
                {
                    foundNonEmpty = true;
                    break;
                }
            }
            VERIFY_IS_TRUE(foundNonEmpty);
        }

        // Negative: null images pointer must fail.
        {
            uint32_t count = 0;
            VERIFY_ARE_EQUAL(WslcListSessionImages(m_defaultSession, nullptr, &count), E_POINTER);
        }

        // Negative: null count pointer must fail.
        {
            WslcImageInfo* images = nullptr;
            VERIFY_ARE_EQUAL(WslcListSessionImages(m_defaultSession, &images, nullptr), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(LoadImage)
    {
        // Positive: load a saved image tar and verify the image can be run.
        {
            // Remove the image first (ignore failure if it wasn't present).
            WslcDeleteSessionImage(m_defaultSession, "hello-world:latest", nullptr);

            std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

            VERIFY_SUCCEEDED(WslcLoadSessionImage(
                m_defaultSession, imageTarFileHandle.get(), static_cast<uint64_t>(fileSize.QuadPart), nullptr, nullptr));

            // Verify the loaded image is usable.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:latest", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Positive: load a saved image tar and verify the image can be run.
        {
            // Remove the image first (ignore failure if it wasn't present).
            WslcDeleteSessionImage(m_defaultSession, "hello-world:latest", nullptr);

            std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");

            VERIFY_SUCCEEDED(WslcLoadSessionImageFromFile(m_defaultSession, imageTar.c_str(), nullptr, nullptr));

            // Verify the loaded image is usable.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:latest", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        WslcLoadImageOptions opts{};

        // Negative: null ImageHandle must fail.
        VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, nullptr, 1, &opts, nullptr), E_INVALIDARG);

        // Negative: INVALID_HANDLE_VALUE must fail.
        VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, INVALID_HANDLE_VALUE, 1, &opts, nullptr), E_INVALIDARG);

        // Negative: zero ContentLength must fail.
        VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, GetCurrentThreadEffectiveToken(), 0, &opts, nullptr), E_INVALIDARG);

        // Negative: null path must fail.
        VERIFY_ARE_EQUAL(WslcLoadSessionImageFromFile(m_defaultSession, nullptr, &opts, nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(ImportImage)
    {
        const auto exportedImageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        constexpr auto c_handleImportedImageName = "my-hello-world-handle:test";
        constexpr auto c_pathImportedImageName = "my-hello-world-path:test";

        // Positive: import an exported image tar via handle+length and verify the image can be run.
        {
            WslcDeleteSessionImage(m_defaultSession, c_handleImportedImageName, nullptr);

            auto cleanup = wil::scope_exit(
                [this]() { LOG_IF_FAILED(WslcDeleteSessionImage(m_defaultSession, c_handleImportedImageName, nullptr)); });

            wil::unique_handle imageTarFileHandle{CreateFileW(
                exportedImageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

            VERIFY_SUCCEEDED(WslcImportSessionImage(
                m_defaultSession, c_handleImportedImageName, imageTarFileHandle.get(), static_cast<uint64_t>(fileSize.QuadPart), nullptr, nullptr));

            auto output = RunContainerAndCapture(m_defaultSession, c_handleImportedImageName, {"/hello"});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Positive: import an exported image tar via path and verify the image can be run.
        {

            WslcDeleteSessionImage(m_defaultSession, c_pathImportedImageName, nullptr);

            auto cleanup = wil::scope_exit(
                [this]() { LOG_IF_FAILED(WslcDeleteSessionImage(m_defaultSession, c_pathImportedImageName, nullptr)); });

            VERIFY_SUCCEEDED(WslcImportSessionImageFromFile(m_defaultSession, c_pathImportedImageName, exportedImageTar.c_str(), nullptr, nullptr));

            auto output = RunContainerAndCapture(m_defaultSession, c_pathImportedImageName, {"/hello"});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        WslcImportImageOptions opts{};

        // Negative: null image name must fail.
        VERIFY_ARE_EQUAL(WslcImportSessionImageFromFile(m_defaultSession, nullptr, exportedImageTar.c_str(), &opts, nullptr), E_POINTER);

        // Negative: missing file input must fail.
        VERIFY_ARE_EQUAL(WslcImportSessionImageFromFile(m_defaultSession, "missing-file-input:test", nullptr, &opts, nullptr), E_POINTER);

        // Negative: zero ContentLength must fail.
        VERIFY_ARE_EQUAL(WslcImportSessionImage(m_defaultSession, "zero-length:test", GetCurrentThreadEffectiveToken(), 0, &opts, nullptr), E_INVALIDARG);
    }

    WSLC_TEST_METHOD(LoadImageNonTar)
    {
        // The load should fail but it just silently ignores the load currently.
        SKIP_TEST_NOT_IMPL();

        // Negative: attempt to load a non-tar file.
        {
            std::filesystem::path pathToSelf = wil::QueryFullProcessImageNameW<std::wstring>(GetCurrentProcess());
            wil::unique_handle selfFileHandle{
                CreateFileW(pathToSelf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == selfFileHandle.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(selfFileHandle.get(), &fileSize));

            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(
                WslcLoadSessionImage(m_defaultSession, selfFileHandle.get(), static_cast<uint64_t>(fileSize.QuadPart), nullptr, &errorMsg), E_FAIL);
            VERIFY_IS_NOT_NULL(errorMsg.get());
        }
    }

    WSLC_TEST_METHOD(ImportImageNonTar)
    {
        // Negative: attempt to load a non-tar file.
        {
            std::filesystem::path pathToSelf = wil::QueryFullProcessImageNameW<std::wstring>(GetCurrentProcess());
            wil::unique_handle selfFileHandle{
                CreateFileW(pathToSelf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == selfFileHandle.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(selfFileHandle.get(), &fileSize));

            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(
                WslcImportSessionImage(
                    m_defaultSession, "import-self:test", selfFileHandle.get(), static_cast<uint64_t>(fileSize.QuadPart), nullptr, &errorMsg),
                E_FAIL);
            VERIFY_IS_NOT_NULL(errorMsg.get());
            LogInfo("Import error: %ws", errorMsg.get());
        }
    }

    WSLC_TEST_METHOD(ImageDelete)
    {
        auto checkForImage = [this](std::string_view image) -> bool {
            WslcImageInfo* images = nullptr;
            uint32_t count = 0;
            VERIFY_SUCCEEDED(WslcListSessionImages(m_defaultSession, &images, &count));
            auto cleanupImages = wil::scope_exit([images]() { CoTaskMemFree(images); });
            bool found = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (images[i].name == image)
                {
                    found = true;
                    break;
                }
            }
            return found;
        };

        // Setup: load hello-world:latest so we have something to delete.
        LoadTestImage("hello-world:latest");

        VERIFY_IS_TRUE(checkForImage("hello-world:latest"));

        // Positive: delete an existing image.
        wil::unique_cotaskmem_string errorMsg;
        VERIFY_SUCCEEDED(WslcDeleteSessionImage(m_defaultSession, "hello-world:latest", &errorMsg));

        // Verify the image is no longer present in the list.
        VERIFY_IS_FALSE(checkForImage("hello-world:latest"));

        // Negative: null name must fail.
        VERIFY_ARE_EQUAL(WslcDeleteSessionImage(m_defaultSession, nullptr, nullptr), E_POINTER);
    }

    // -----------------------------------------------------------------------
    // Container lifecycle tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(CreateContainer)
    {
        // Simple echo — verify stdout is captured correctly.
        {
            auto output = RunContainerAndCapture(m_defaultSession, "debian:latest", {"/bin/echo", "OK"});
            VERIFY_ARE_EQUAL(output.stdoutOutput, "OK\n");
            VERIFY_ARE_EQUAL(output.stderrOutput, "");
        }

        // Verify stdout and stderr are routed independently.
        {
            auto output =
                RunContainerAndCapture(m_defaultSession, "debian:latest", {"/bin/sh", "-c", "echo stdout && echo stderr >&2"});
            VERIFY_ARE_EQUAL(output.stdoutOutput, "stdout\n");
            VERIFY_ARE_EQUAL(output.stderrOutput, "stderr\n");
        }

        // Verify that creating a container with a non-existent image fails.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("invalid-image:notfound", &containerSettings));

            WslcContainer container = nullptr;
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(WslcCreateContainer(m_defaultSession, &containerSettings, &container, &errorMsg), WSLC_E_IMAGE_NOT_FOUND);
            VERIFY_IS_NULL(container);
        }

        // Verify that a null image name is rejected.
        {
            WslcContainerSettings containerSettings;
            VERIFY_ARE_EQUAL(WslcInitContainerSettings(nullptr, &containerSettings), E_POINTER);
        }

        // Verify that a null settings pointer is rejected.
        {
            VERIFY_ARE_EQUAL(WslcInitContainerSettings("debian:latest", nullptr), E_POINTER);
        }

        // Verify that a null container output pointer is rejected.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcCreateContainer(m_defaultSession, &containerSettings, nullptr, nullptr), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(ContainerGetID)
    {
        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));

        // Positive: ID is returned and is the expected length of hex characters.
        CHAR id[WSLC_CONTAINER_ID_BUFFER_SIZE]{};
        VERIFY_SUCCEEDED(WslcGetContainerID(container.get(), id));
        VERIFY_ARE_EQUAL(strnlen(id, WSLC_CONTAINER_ID_BUFFER_SIZE), static_cast<size_t>(WSLC_CONTAINER_ID_BUFFER_SIZE - 1));

        // Negative: null ID buffer must fail.
        VERIFY_ARE_EQUAL(WslcGetContainerID(container.get(), nullptr), E_POINTER);

        VERIFY_SUCCEEDED(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr));
    }

    WSLC_TEST_METHOD(ContainerGetState)
    {
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));

        // State after creation: CREATED.
        {
            WslcContainerState state{};
            VERIFY_SUCCEEDED(WslcGetContainerState(container.get(), &state));
            VERIFY_ARE_EQUAL(state, WSLC_CONTAINER_STATE_CREATED);
        }

        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        // State while running: RUNNING.
        {
            WslcContainerState state{};
            VERIFY_SUCCEEDED(WslcGetContainerState(container.get(), &state));
            VERIFY_ARE_EQUAL(state, WSLC_CONTAINER_STATE_RUNNING);
        }

        VERIFY_SUCCEEDED(WslcStopContainer(container.get(), WSLC_SIGNAL_SIGKILL, 0, nullptr));

        // State after stop: EXITED.
        {
            WslcContainerState state{};
            VERIFY_SUCCEEDED(WslcGetContainerState(container.get(), &state));
            VERIFY_ARE_EQUAL(state, WSLC_CONTAINER_STATE_EXITED);
        }

        // Negative: null state pointer must fail.
        VERIFY_ARE_EQUAL(WslcGetContainerState(container.get(), nullptr), E_POINTER);

        VERIFY_SUCCEEDED(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr));
    }

    WSLC_TEST_METHOD(ContainerStopAndDelete)
    {
        // Build a long-running container.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "999"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsName(&containerSettings, "wslc-stop-delete-test"));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        // Acquire and release the init process handle — we won't read its I/O.
        {
            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));
        }

        // Stop the container gracefully (after the timeout).
        VERIFY_SUCCEEDED(WslcStopContainer(container.get(), WSLC_SIGNAL_SIGKILL, 0, nullptr));

        // Delete the stopped container.
        VERIFY_SUCCEEDED(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr));
    }

    WSLC_TEST_METHOD(ProcessIOHandles)
    {
        // Verify that stdout and stderr can each be read, and are independent streams.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "printf 'stdout-line\n' ; printf 'stderr-line\n' >&2"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsFlags(&containerSettings, WSLC_CONTAINER_FLAG_NONE));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(process.get(), &exitEvent));

        HANDLE rawStdout = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &rawStdout));
        wil::unique_handle ownedStdout(rawStdout);

        HANDLE rawStderr = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &rawStderr));
        wil::unique_handle ownedStderr(rawStderr);

        // Verify that each handle can only be acquired once.
        {
            HANDLE duplicate = nullptr;
            VERIFY_ARE_EQUAL(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &duplicate), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 60 * 1000), WAIT_OBJECT_0);
    }

    WSLC_TEST_METHOD(ContainerNetworkingMode)
    {
        // BRIDGED: container should have an eth0 interface in sysfs.
        {
            auto output = RunContainerAndCapture(
                m_defaultSession,
                "debian:latest",
                {"/bin/sh", "-c", "[ -d /sys/class/net/eth0 ] && echo 'HAS_ETH0' || echo 'NO_ETH0'"},
                WSLC_CONTAINER_FLAG_NONE,
                nullptr,
                60s,
                WSLC_CONTAINER_NETWORKING_MODE_BRIDGED);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "HAS_ETH0\n");
        }

        // NONE: container should not have an eth0 interface.
        {
            auto output = RunContainerAndCapture(
                m_defaultSession,
                "debian:latest",
                {"/bin/sh", "-c", "[ -d /sys/class/net/eth0 ] && echo 'HAS_ETH0' || echo 'NO_ETH0'"},
                WSLC_CONTAINER_FLAG_NONE,
                nullptr,
                60s,
                WSLC_CONTAINER_NETWORKING_MODE_NONE);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "NO_ETH0\n");
        }

        // Invalid networking mode must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsNetworkingMode(&containerSettings, static_cast<WslcContainerNetworkingMode>(99)), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerPortMapping)
    {
        // Negative: null mappings with nonzero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsPortMappings(&containerSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            WslcContainerPortMapping portMappings[1] = {};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsPortMappings(&containerSettings, portMappings, 0), E_INVALIDARG);
        }

        // Positive: null mappings with zero count must succeed (clears the mapping).
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsPortMappings(&containerSettings, nullptr, 0));
        }

        // Negative: port mappings with NONE networking must fail at container creation.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_NONE));

            WslcContainerPortMapping mapping{};
            mapping.windowsPort = 12342;
            mapping.containerPort = 8000;
            mapping.protocol = WSLC_PORT_PROTOCOL_TCP;
            VERIFY_SUCCEEDED(WslcSetContainerSettingsPortMappings(&containerSettings, &mapping, 1));

            WslcContainer rawContainer = nullptr;
            VERIFY_ARE_EQUAL(WslcCreateContainer(m_defaultSession, &containerSettings, &rawContainer, nullptr), E_INVALIDARG);
            VERIFY_IS_NULL(rawContainer);
        }

        // Functional: create a container with BRIDGED networking and a port mapping;
        // verify that a TCP connection from the host reaches the container.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"python3", "-m", "http.server", "8000"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
            const char* env[] = {"PYTHONUNBUFFERED=1"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsEnvVariables(&procSettings, env, ARRAYSIZE(env)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("python:3.12-alpine", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_BRIDGED));

            WslcContainerPortMapping mapping{};
            mapping.windowsPort = 12341;
            mapping.containerPort = 8000;
            mapping.protocol = WSLC_PORT_PROTOCOL_TCP;
            VERIFY_SUCCEEDED(WslcSetContainerSettingsPortMappings(&containerSettings, &mapping, 1));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

            wil::unique_handle ownedStdout;
            VERIFY_SUCCEEDED(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

            WaitForOutput(std::move(ownedStdout), "Serving HTTP on", 30s);

            ExpectHttpResponse(L"http://127.0.0.1:12341", 200);
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeUnit)
    {
        // Negative: null volumes with nonzero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, 0), E_INVALIDARG);
        }

        // Negative: null paths must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {nullptr, "/mnt/path"};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), nullptr};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        // Relative paths must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {L"relative", "/mnt/path"};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), "./mnt/path"};
            VERIFY_ARE_EQUAL(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        // Positive: null volumes with zero count must succeed (clears volumes).
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsVolumes(&containerSettings, nullptr, 0));
        }

        // Absolute paths should succeed
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), "/mnt/path"};
            VERIFY_SUCCEEDED(WslcSetContainerSettingsVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)));
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeFunctional)
    {
        // Functional: mount a read-write and a read-only directory into the container.
        {
            auto hostRwDir = std::filesystem::current_path() / "wslc-test-vol-rw";
            auto hostRoDir = std::filesystem::current_path() / "wslc-test-vol-ro";
            std::filesystem::create_directories(hostRwDir);
            std::filesystem::create_directories(hostRoDir);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                std::error_code ec;
                std::filesystem::remove_all(hostRwDir, ec);
                std::filesystem::remove_all(hostRoDir, ec);
            });

            // Write sentinel files into both host directories.
            {
                std::ofstream rwSentinel(hostRwDir / "hello.txt");
                rwSentinel << "hello-rw";
            }
            {
                std::ofstream roSentinel(hostRoDir / "hello.txt");
                roSentinel << "hello-ro";
            }

            WslcContainerVolume volumes[2]{};
            volumes[0].windowsPath = hostRwDir.c_str();
            volumes[0].containerPath = "/mnt/rw";
            volumes[0].readOnly = FALSE;
            volumes[1].windowsPath = hostRoDir.c_str();
            volumes[1].containerPath = "/mnt/ro";
            volumes[1].readOnly = TRUE;

            // Container script:
            //   1. Read from the rw mount.
            //   2. Read from the ro mount.
            //   3. Write a file to the rw mount; print WRITE_OK on success.
            //   4. Try to write to the ro mount; print RO_WRITE_BLOCKED if correctly rejected.
            const char* script =
                "cat /mnt/rw/hello.txt && "
                "cat /mnt/ro/hello.txt && "
                "echo 'container-write' > /mnt/rw/written.txt && echo 'WRITE_OK' && "
                "if touch /mnt/ro/probe 2>/dev/null; then echo 'RO_WRITE_ALLOWED'; else echo 'RO_WRITE_BLOCKED'; fi";

            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", script};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsVolumes(&containerSettings, volumes, 2));

            ProcessOutput output = RunContainerAndCapture(m_defaultSession, containerSettings);

            // Verify all four outcomes.
            VERIFY_IS_TRUE(output.stdoutOutput.find("hello-rw") != std::string::npos);
            VERIFY_IS_TRUE(output.stdoutOutput.find("hello-ro") != std::string::npos);
            VERIFY_IS_TRUE(output.stdoutOutput.find("WRITE_OK") != std::string::npos);
            VERIFY_IS_TRUE(output.stdoutOutput.find("RO_WRITE_BLOCKED") != std::string::npos);
            VERIFY_IS_TRUE(output.stdoutOutput.find("RO_WRITE_ALLOWED") == std::string::npos);

            // Verify the file written by the container is visible on the host.
            std::ifstream written(hostRwDir / "written.txt");
            VERIFY_IS_TRUE(written.is_open());
            std::string writtenContent((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
            VERIFY_ARE_EQUAL(writtenContent, "container-write\n");
        }
    }

    WSLC_TEST_METHOD(ContainerInspect)
    {
        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));

        wil::unique_cotaskmem_ansistring inspectData;
        VERIFY_SUCCEEDED(WslcInspectContainer(container.get(), &inspectData));

        VERIFY_IS_NOT_NULL(inspectData);

        auto inspectObject = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectContainer>(inspectData.get());

        CHAR containerId[WSLC_CONTAINER_ID_BUFFER_SIZE];
        VERIFY_SUCCEEDED(WslcGetContainerID(container.get(), containerId));

        VERIFY_ARE_EQUAL(containerId, inspectObject.Id);
    }

    WSLC_TEST_METHOD(ContainerExec)
    {
        // Start a long-running container so we can exec into it.
        WslcProcessSettings initProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&initProcSettings));
        const char* initArgv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&initProcSettings, initArgv, ARRAYSIZE(initArgv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &initProcSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        // Positive: exec an echo command and verify its output.
        {
            WslcProcessSettings execProcSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&execProcSettings));
            const char* execArgv[] = {"/bin/echo", "exec-hello"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execProcSettings, execArgv, ARRAYSIZE(execArgv)));

            UniqueProcess execProcess;
            VERIFY_SUCCEEDED(WslcCreateContainerProcess(container.get(), &execProcSettings, &execProcess, nullptr));

            auto output = WaitForProcessOutput(execProcess.get());
            VERIFY_ARE_EQUAL(output.stdoutOutput, "exec-hello\n");
        }

        // Negative: process settings with no command line must fail.
        {
            WslcProcessSettings emptyProcSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&emptyProcSettings));
            WslcProcess newProcess = nullptr;
            VERIFY_ARE_EQUAL(WslcCreateContainerProcess(container.get(), &emptyProcSettings, &newProcess, nullptr), E_INVALIDARG);
            VERIFY_IS_NULL(newProcess);
        }

        // Negative: null newProcess output pointer must fail.
        {
            WslcProcessSettings execProcSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&execProcSettings));
            const char* execArgv[] = {"/bin/echo", "x"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execProcSettings, execArgv, ARRAYSIZE(execArgv)));
            VERIFY_ARE_EQUAL(WslcCreateContainerProcess(container.get(), &execProcSettings, nullptr, nullptr), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(ContainerHostName)
    {
        // Unit: setting a hostname succeeds.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsHostName(&containerSettings, "test-host"));
        }

        // Functional: container process should see the configured hostname.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/hostname"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsHostName(&containerSettings, "my-test-host"));

            auto output = RunContainerAndCapture(m_defaultSession, containerSettings);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "my-test-host\n");
        }
    }

    WSLC_TEST_METHOD(ContainerDomainName)
    {
        // Unit: setting a domain name succeeds.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsDomainName(&containerSettings, "my.domain"));
        }

        // Functional: container should see the configured domain name.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", "echo $(domainname)"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsDomainName(&containerSettings, "test.local"));

            auto output = RunContainerAndCapture(m_defaultSession, containerSettings);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "test.local\n");
        }
    }

    WSLC_TEST_METHOD(ProcessEnvVariables)
    {
        // Negative: null pointer with nonzero count must fail.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsEnvVariables(&procSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* envVars[] = {"FOO=bar"};
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsEnvVariables(&procSettings, envVars, 0), E_INVALIDARG);
        }

        // Positive: null pointer with zero count must succeed (clears env vars).
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsEnvVariables(&procSettings, nullptr, 0));
        }

        // Functional: set an env var and verify it is visible inside the container.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", "echo $MY_TEST_VAR"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
            const char* envVars[] = {"MY_TEST_VAR=hello-from-test"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsEnvVariables(&procSettings, envVars, ARRAYSIZE(envVars)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            auto output = RunContainerAndCapture(m_defaultSession, containerSettings);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "hello-from-test\n");
        }
    }

    WSLC_TEST_METHOD(ProcessSignal)
    {
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(process.get(), &exitEvent));

        // Positive: SIGKILL the running process.
        VERIFY_SUCCEEDED(WslcSignalProcess(process.get(), WSLC_SIGNAL_SIGKILL));

        // The process exit event should fire after the signal.
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // Negative: null process handle must return an error.
        VERIFY_ARE_EQUAL(WslcSignalProcess(nullptr, WSLC_SIGNAL_SIGKILL), E_POINTER);
    }

    WSLC_TEST_METHOD(ProcessGetPid)
    {
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        // Positive: PID of a running process must be non-zero.
        uint32_t pid = 0;
        VERIFY_SUCCEEDED(WslcGetProcessPid(process.get(), &pid));
        VERIFY_IS_TRUE(pid > 0);

        // Negative: null pid pointer must fail.
        VERIFY_ARE_EQUAL(WslcGetProcessPid(process.get(), nullptr), E_POINTER);

        // Negative: null process handle must return an error.
        WslcProcess nullProcess = nullptr;
        VERIFY_ARE_EQUAL(WslcGetProcessPid(nullProcess, &pid), E_POINTER);
    }

    WSLC_TEST_METHOD(ProcessGetExitCode)
    {
        auto RunAndGetProcess = [&](int exitCodeArg) -> UniqueProcess {
            std::string script = "exit " + std::to_string(exitCodeArg);
            const char* argv[] = {"/bin/sh", "-c", script.c_str()};

            WslcProcessSettings procSettings;
            THROW_IF_FAILED(WslcInitProcessSettings(&procSettings));
            THROW_IF_FAILED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            THROW_IF_FAILED(WslcInitContainerSettings("debian:latest", &containerSettings));
            THROW_IF_FAILED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            UniqueContainer container;
            THROW_IF_FAILED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
            THROW_IF_FAILED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

            UniqueProcess process;
            THROW_IF_FAILED(WslcGetContainerInitProcess(container.get(), &process));

            HANDLE exitEvent = nullptr;
            THROW_IF_FAILED(WslcGetProcessExitEvent(process.get(), &exitEvent));
            THROW_HR_IF(HRESULT_FROM_WIN32(WAIT_TIMEOUT), WaitForSingleObject(exitEvent, 30 * 1000) != WAIT_OBJECT_0);

            return process;
        };

        auto RunAndGetExitCode = [&](int exitCodeArg) -> INT32 {
            UniqueProcess process = RunAndGetProcess(exitCodeArg);

            INT32 code = -1;
            THROW_IF_FAILED(WslcGetProcessExitCode(process.get(), &code));
            return code;
        };

        // Positive: verify exit 0 and exit 42 are reported correctly.
        VERIFY_ARE_EQUAL(RunAndGetExitCode(0), 0);
        VERIFY_ARE_EQUAL(RunAndGetExitCode(42), 42);

        // Negative: null exit code pointer must fail.
        {
            auto process = RunAndGetProcess(0);
            VERIFY_ARE_EQUAL(WslcGetProcessExitCode(process.get(), nullptr), E_POINTER);
        }

        // Negative: null process handle must return an error.
        {
            WslcProcess nullProcess = nullptr;
            INT32 code = 0;
            VERIFY_ARE_EQUAL(WslcGetProcessExitCode(nullProcess, &code), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(ProcessGetState)
    {
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(process.get(), &exitEvent));

        // State while running: RUNNING.
        {
            WslcProcessState state{};
            VERIFY_SUCCEEDED(WslcGetProcessState(process.get(), &state));
            VERIFY_ARE_EQUAL(state, WSLC_PROCESS_STATE_RUNNING);
        }

        // Bonus test for exit code while running
        {
            INT32 exitCode{};
            VERIFY_ARE_EQUAL(WslcGetProcessExitCode(process.get(), &exitCode), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
            VERIFY_ARE_EQUAL(exitCode, -1);
        }

        // Kill the process and wait for the exit event.
        VERIFY_SUCCEEDED(WslcSignalProcess(process.get(), WSLC_SIGNAL_SIGKILL));
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // State after kill: SIGNALLED or EXITED.
        {
            WslcProcessState state{};
            VERIFY_SUCCEEDED(WslcGetProcessState(process.get(), &state));
            VERIFY_IS_TRUE(state == WSLC_PROCESS_STATE_SIGNALLED || state == WSLC_PROCESS_STATE_EXITED);
        }

        // Negative: null state pointer must fail.
        VERIFY_ARE_EQUAL(WslcGetProcessState(process.get(), nullptr), E_POINTER);

        // Negative: null process handle must return an error.
        {
            WslcProcess nullProcess = nullptr;
            WslcProcessState state{};
            VERIFY_ARE_EQUAL(WslcGetProcessState(nullProcess, &state), E_POINTER);
        }
    }

    WSLC_TEST_METHOD(ProcessCurrentDirectory)
    {
        // Unit: setting a current directory returns S_OK.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCurrentDirectory(&procSettings, "/tmp"));
        }

        // Negative: null processSettings must fail.
        VERIFY_ARE_EQUAL(WslcSetProcessSettingsCurrentDirectory(nullptr, "/tmp"), E_POINTER);

        // Functional: verify pwd reports the configured working directory.
        {
            auto output = RunContainerAndCapture(m_defaultSession, "debian:latest", {"/bin/pwd"});
            // Default working directory baseline — just verify pwd succeeds.
            VERIFY_IS_FALSE(output.stdoutOutput.empty());
        }

        // Functional: set current directory to /tmp and verify pwd output.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/pwd"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCurrentDirectory(&procSettings, "/tmp"));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            auto output = RunContainerAndCapture(m_defaultSession, containerSettings);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "/tmp\n");
        }
    }

    WSLC_TEST_METHOD(GetVersion)
    {
        // Positive: returns S_OK and fills in a non-zero version.
        {
            WslcVersion version{};
            VERIFY_SUCCEEDED(WslcGetVersion(&version));
            VERIFY_IS_TRUE(version.major > 0 || version.minor > 0 || version.revision > 0);
        }

        // Negative: null pointer must fail.
        VERIFY_ARE_EQUAL(WslcGetVersion(nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(CanRun)
    {
        BOOL canRun = FALSE;
        WslcComponentFlags missing{};
        VERIFY_SUCCEEDED(WslcCanRun(&canRun, &missing));

        // Presumably anywhere that we run the tests we should get these results.
        // The levels of OS state modification required to test beyond this are beyond the scope of these tests.
        VERIFY_ARE_EQUAL(canRun, TRUE);
        VERIFY_ARE_EQUAL(missing, WSLC_COMPONENT_FLAG_NONE);
    }

    // -----------------------------------------------------------------------
    // WslcSetProcessSettingsCallbacks tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(ProcessIoCallbackUnit)
    {
        auto noopIoCb = [](WslcProcessIOHandle, const BYTE*, uint32_t, PVOID) {};
        auto noopExitCb = [](INT32, PVOID) {};

        // Negative: null processSettings must fail.
        {
            WslcProcessCallbacks callbacks{};
            callbacks.onStdOut = noopIoCb;
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsCallbacks(nullptr, &callbacks, nullptr), E_POINTER);
        }

        // Negative: null callbacks pointer with non-null context must fail.
        {
            int ctx = 0;
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsCallbacks(&procSettings, nullptr, &ctx), E_INVALIDARG);
        }

        // Positive: null callbacks pointer with null context clears all callbacks.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, nullptr, nullptr));
        }

        // Positive: set onStdOut only.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            WslcProcessCallbacks callbacks{};
            callbacks.onStdOut = noopIoCb;
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, nullptr));
        }

        // Positive: set onStdErr only.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            WslcProcessCallbacks callbacks{};
            callbacks.onStdErr = noopIoCb;
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, nullptr));
        }

        // Positive: set onExit only.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            WslcProcessCallbacks callbacks{};
            callbacks.onExit = noopExitCb;
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, nullptr));
        }

        // Positive: set all three callbacks with a context.
        {
            int ctx = 0;
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            WslcProcessCallbacks callbacks{};
            callbacks.onStdOut = noopIoCb;
            callbacks.onStdErr = noopIoCb;
            callbacks.onExit = noopExitCb;
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, &ctx));
        }

        // Negative: StartContainer without ATTACH fails when callbacks are registered.
        // The ATTACH flag is required so the IOCallback can claim the init process pipe handles.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sleep", "1"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
            WslcProcessCallbacks callbacks{};
            callbacks.onStdOut = noopIoCb;
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, nullptr));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_ARE_EQUAL(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ProcessIoCallbackInitProcess)
    {
        struct IOContext
        {
            std::string stdoutData;
            std::string stderrData;
        } ioContext;

        // Both streams share one callback; ioHandle distinguishes which accumulator to use.
        auto ioCb = [](WslcProcessIOHandle ioHandle, const BYTE* data, uint32_t size, PVOID ctx) {
            auto* c = static_cast<IOContext*>(ctx);
            auto& target = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? c->stdoutData : c->stderrData;
            target.append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "echo STDOUT && echo STDERR >&2"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcProcessCallbacks callbacks{};
        callbacks.onStdOut = ioCb;
        callbacks.onStdErr = ioCb;
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, &ioContext));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(process.get(), &exitEvent));
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // Release the process handle first, then the container handle.
        // Releasing the container destroys the WslcContainerImpl which joins the IOCallback
        // thread, guaranteeing all bytes have been delivered before the assertions below.
        process.reset();
        container.reset();

        VERIFY_ARE_EQUAL(ioContext.stdoutData, "STDOUT\n");
        VERIFY_ARE_EQUAL(ioContext.stderrData, "STDERR\n");
    }

    WSLC_TEST_METHOD(ProcessIoCallbackExecProcess)
    {
        // Start a long-running container so we can exec into it.
        WslcProcessSettings initProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&initProcSettings));
        const char* initArgv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&initProcSettings, initArgv, ARRAYSIZE(initArgv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &initProcSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        struct IOContext
        {
            std::string stdoutData;
            std::string stderrData;
        } ioContext;

        auto ioCb = [](WslcProcessIOHandle ioHandle, const BYTE* data, uint32_t size, PVOID ctx) {
            auto* c = static_cast<IOContext*>(ctx);
            auto& target = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? c->stdoutData : c->stderrData;
            target.append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings execProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&execProcSettings));
        const char* execArgv[] = {"/bin/sh", "-c", "echo EXEC_OUT && echo EXEC_ERR >&2"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execProcSettings, execArgv, ARRAYSIZE(execArgv)));

        WslcProcessCallbacks callbacks{};
        callbacks.onStdOut = ioCb;
        callbacks.onStdErr = ioCb;
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&execProcSettings, &callbacks, &ioContext));

        UniqueProcess execProcess;
        VERIFY_SUCCEEDED(WslcCreateContainerProcess(container.get(), &execProcSettings, &execProcess, nullptr));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(execProcess.get(), &exitEvent));
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // Releasing the exec process handle destroys WslcProcessImpl and joins its IOCallback
        // thread, ensuring all bytes are delivered before assertions.
        execProcess.reset();

        VERIFY_ARE_EQUAL(ioContext.stdoutData, "EXEC_OUT\n");
        VERIFY_ARE_EQUAL(ioContext.stderrData, "EXEC_ERR\n");
    }

    WSLC_TEST_METHOD(ProcessIoCallbackHandleExclusion)
    {
        // Register a stdout callback only. IOCallback always acquires ALL pipe handles
        // (draining uncallbacked streams to prevent deadlock), so both stdout and stderr
        // handles are consumed and neither can be obtained via WslcGetProcessIOHandle.
        auto noopIoCb = [](WslcProcessIOHandle, const BYTE*, uint32_t, PVOID) {};

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcProcessCallbacks callbacks{};
        callbacks.onStdOut = noopIoCb;
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, nullptr));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        // stdout handle was consumed by the IOCallback — must not be obtainable.
        {
            HANDLE h = nullptr;
            VERIFY_ARE_EQUAL(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &h), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // stderr handle was also consumed in order to drain it despite not being given a callback.
        {
            HANDLE h = nullptr;
            VERIFY_ARE_EQUAL(WslcGetProcessIOHandle(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &h), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    WSLC_TEST_METHOD(ProcessIoCallbackExitCallback)
    {
        // Verify the onExit callback fires with the correct exit code after IO has been flushed.
        // We test both exit 0 and a non-zero exit code.
        auto RunAndCaptureExit = [&](int exitCodeArg) -> std::pair<INT32, std::string> {
            std::string stdoutData;
            std::atomic<INT32> capturedExitCode{-999};

            struct Context
            {
                std::string* stdoutData;
                std::atomic<INT32>* capturedExitCode;
                wil::unique_event exitEvent{wil::EventOptions::ManualReset};
            } ctx{&stdoutData, &capturedExitCode};

            auto ioCb = [](WslcProcessIOHandle, const BYTE* data, uint32_t size, PVOID c) {
                static_cast<Context*>(c)->stdoutData->append(reinterpret_cast<const char*>(data), size);
            };
            auto exitCb = [](INT32 code, PVOID c) {
                static_cast<Context*>(c)->capturedExitCode->store(code);
                static_cast<Context*>(c)->exitEvent.SetEvent();
            };

            std::string script = "echo HELLO && exit " + std::to_string(exitCodeArg);
            const char* argv[] = {"/bin/sh", "-c", script.c_str()};

            WslcProcessSettings procSettings;
            THROW_IF_FAILED(WslcInitProcessSettings(&procSettings));
            THROW_IF_FAILED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcProcessCallbacks callbacks{};
            callbacks.onStdOut = ioCb;
            callbacks.onExit = exitCb;
            THROW_IF_FAILED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, &ctx));

            WslcContainerSettings containerSettings;
            THROW_IF_FAILED(WslcInitContainerSettings("debian:latest", &containerSettings));
            THROW_IF_FAILED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            UniqueContainer container;
            THROW_IF_FAILED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
            THROW_IF_FAILED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

            UniqueProcess process;
            THROW_IF_FAILED(WslcGetContainerInitProcess(container.get(), &process));

            THROW_HR_IF(HRESULT_FROM_WIN32(WAIT_TIMEOUT), WaitForSingleObject(ctx.exitEvent.get(), 60 * 1000) != WAIT_OBJECT_0);

            return {capturedExitCode.load(), stdoutData};
        };

        // Exit 0: onExit must fire with code 0; IO must have been delivered first.
        {
            auto [exitCode, output] = RunAndCaptureExit(0);
            VERIFY_ARE_EQUAL(exitCode, 0);
            VERIFY_ARE_EQUAL(output, "HELLO\n");
        }

        // Non-zero exit: onExit must report the correct code.
        {
            auto [exitCode, output] = RunAndCaptureExit(42);
            VERIFY_ARE_EQUAL(exitCode, 42);
            VERIFY_ARE_EQUAL(output, "HELLO\n");
        }
    }

    WSLC_TEST_METHOD(ProcessIoCallbackCancelOnRelease)
    {
        // Verify that releasing the process handle while an exec'd process is still running
        // and writing IO cancels the IOCallback pump:
        //   - No IO callbacks arrive after the handle is released.
        //   - onExit is never invoked (cancellation returns runResult=false, suppressing it).
        //
        // A secondary (exec'd) process is used so that the long-lived init process keeps the
        // container alive, allowing UniqueContainer to clean up normally at scope exit.

        // Start a long-running init process to keep the container alive.
        WslcProcessSettings initProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&initProcSettings));
        const char* initArgv[] = {"/bin/sleep", "999"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&initProcSettings, initArgv, ARRAYSIZE(initArgv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &initProcSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        struct Context
        {
            std::atomic<int> callbackCount{0};
            std::atomic<bool> exitFired{false};
        } ctx;

        auto ioCb = [](WslcProcessIOHandle, const BYTE*, uint32_t, PVOID c) {
            static_cast<Context*>(c)->callbackCount.fetch_add(1);
        };
        auto exitCb = [](INT32, PVOID c) { static_cast<Context*>(c)->exitFired.store(true); };

        // Continuous writer: emits one line every 50 ms indefinitely.
        WslcProcessSettings execProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&execProcSettings));
        const char* execArgv[] = {"/bin/sh", "-c", "while true; do echo LINE; sleep 0.05; done"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execProcSettings, execArgv, ARRAYSIZE(execArgv)));

        WslcProcessCallbacks callbacks{};
        callbacks.onStdOut = ioCb;
        callbacks.onExit = exitCb;
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&execProcSettings, &callbacks, &ctx));

        UniqueProcess execProcess;
        VERIFY_SUCCEEDED(WslcCreateContainerProcess(container.get(), &execProcSettings, &execProcess, nullptr));

        // Wait long enough for at least several callbacks to arrive.
        Sleep(500);
        VERIFY_IS_TRUE(ctx.callbackCount.load() > 0);

        // Release the exec process handle while the process is still running and writing.
        // This destructs WslcProcessImpl → cancels the IOCallback → joins its thread.
        // By the time execProcess.reset() returns, the pump thread has exited.
        execProcess.reset();

        // Snapshot the count now that the thread is confirmed stopped.
        int countAtRelease = ctx.callbackCount.load();

        // onExit must not have fired: cancellation sets runResult=false, suppressing the call.
        VERIFY_IS_FALSE(ctx.exitFired.load());

        // Wait another interval — no further callbacks can arrive after the thread has joined.
        Sleep(200);
        VERIFY_ARE_EQUAL(ctx.callbackCount.load(), countAtRelease);
        VERIFY_IS_FALSE(ctx.exitFired.load());
    }

    WSLC_TEST_METHOD(ProcessIoCallbackLargeOutput)
    {
        // Generate ~1 MiB of stdout via: dd if=/dev/zero bs=1024 count=1024 | base64
        // 1,048,576 zero bytes → base64 output is 1,398,104 bytes (ceil(1048576/3)*4).
        static constexpr size_t c_expectedBytes = 1'398'104;

        std::string stdoutData;
        stdoutData.reserve(c_expectedBytes + 4096);

        auto ioCb = [](WslcProcessIOHandle, const BYTE* data, uint32_t size, PVOID ctx) {
            static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "dd if=/dev/zero bs=1024 count=1024 2>/dev/null | base64 -w 0"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

        WslcProcessCallbacks callbacks{};
        callbacks.onStdOut = ioCb;
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCallbacks(&procSettings, &callbacks, &stdoutData));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(process.get(), &exitEvent));
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 60 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // Join the IOCallback thread before inspecting the accumulator.
        process.reset();
        container.reset();

        VERIFY_ARE_EQUAL(stdoutData.size(), c_expectedBytes);
    }

    // -----------------------------------------------------------------------
    // Storage tests
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(SessionCreateVhd)
    {
        constexpr auto c_volumeName = "wslc-test-data-vol";
        constexpr auto c_vhdSizeBytes = _1GB;

        std::filesystem::path vhdSessionStorage = m_storagePath / "wslc-vhd-test-storage";
        auto removeStorage = wil::scope_exit([&]() {
            std::error_code error;
            std::filesystem::remove_all(vhdSessionStorage, error);
            if (error)
            {
                LogError("Failed to remove VHD test storage %ws: %hs", vhdSessionStorage.c_str(), error.message().c_str());
            }
        });

        // Create a dedicated session so that volume creation does not affect the shared default session.
        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(L"wslc-vhd-test", vhdSessionStorage.c_str(), &sessionSettings));

        WslcVhdRequirements sessionVhd{};
        sessionVhd.sizeInBytes = 4 * _1GB;
        sessionVhd.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSetSessionSettingsVhd(&sessionSettings, &sessionVhd));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &session, nullptr));

        // Load debian so we have a container image to work with.
        std::filesystem::path debianTar = GetTestImagePath("debian:latest");
        VERIFY_SUCCEEDED(WslcLoadSessionImageFromFile(session.get(), debianTar.c_str(), nullptr, nullptr));

        // Positive: create a named VHD volume in the session.
        {
            WslcVhdRequirements vhd{};
            vhd.name = c_volumeName;
            vhd.sizeInBytes = c_vhdSizeBytes;
            vhd.type = WSLC_VHD_TYPE_DYNAMIC;
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_SUCCEEDED(WslcCreateSessionVhdVolume(session.get(), &vhd, &errorMsg));

            // The backing VHD file must exist on disk.
            std::filesystem::path expectedVhdPath = vhdSessionStorage / "volumes" / (std::string(c_volumeName) + ".vhdx");
            VERIFY_IS_TRUE(std::filesystem::exists(expectedVhdPath));
        }

        // Positive: write a marker via a container that mounts the named volume.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", "echo wslc-vhd-test > /data/marker.txt"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            WslcContainerNamedVolume namedVol{};
            namedVol.name = c_volumeName;
            namedVol.containerPath = "/data";
            namedVol.readOnly = FALSE;
            VERIFY_SUCCEEDED(WslcSetContainerSettingsNamedVolumes(&containerSettings, &namedVol, 1));

            auto output = RunContainerAndCapture(session.get(), containerSettings);
            VERIFY_IS_TRUE(output.stderrOutput.empty());
        }

        // Positive: read back the marker in a second container (read-only mount).
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", "cat /data/marker.txt"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            WslcContainerNamedVolume namedVol{};
            namedVol.name = c_volumeName;
            namedVol.containerPath = "/data";
            namedVol.readOnly = TRUE;
            VERIFY_SUCCEEDED(WslcSetContainerSettingsNamedVolumes(&containerSettings, &namedVol, 1));

            auto output = RunContainerAndCapture(session.get(), containerSettings);
            VERIFY_ARE_EQUAL(output.stdoutOutput, "wslc-vhd-test\n");
        }

        // Positive: delete the volume.
        {
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_SUCCEEDED(WslcDeleteSessionVhdVolume(session.get(), c_volumeName, &errorMsg));

            // The backing VHD file must not exist on disk.
            std::filesystem::path expectedVhdPath = vhdSessionStorage / "volumes" / (std::string(c_volumeName) + ".vhdx");
            VERIFY_IS_FALSE(std::filesystem::exists(expectedVhdPath));
        }

        // Negative: null options pointer must fail.
        VERIFY_ARE_EQUAL(WslcCreateSessionVhdVolume(session.get(), nullptr, nullptr), E_POINTER);

        // Negative: null name must fail.
        {
            WslcVhdRequirements vhd{};
            vhd.name = nullptr;
            vhd.sizeInBytes = c_vhdSizeBytes;
            vhd.type = WSLC_VHD_TYPE_DYNAMIC;
            VERIFY_ARE_EQUAL(WslcCreateSessionVhdVolume(session.get(), &vhd, nullptr), E_INVALIDARG);
        }

        // Negative: zero sizeInBytes must fail.
        {
            WslcVhdRequirements vhd{};
            vhd.name = c_volumeName;
            vhd.sizeInBytes = 0;
            vhd.type = WSLC_VHD_TYPE_DYNAMIC;
            VERIFY_ARE_EQUAL(WslcCreateSessionVhdVolume(session.get(), &vhd, nullptr), E_INVALIDARG);
        }

        // Negative: fixed VHD type is not yet supported.
        {
            WslcVhdRequirements vhd{};
            vhd.name = c_volumeName;
            vhd.sizeInBytes = c_vhdSizeBytes;
            vhd.type = WSLC_VHD_TYPE_FIXED;
            VERIFY_ARE_EQUAL(WslcCreateSessionVhdVolume(session.get(), &vhd, nullptr), E_NOTIMPL);
        }
    }

    // -----------------------------------------------------------------------
    // Stub tests for unimplemented (E_NOTIMPL) functions.
    // Each of these confirms the current state of the SDK; once the underlying
    // function is implemented the assertion below will catch it and the test
    // should be updated to exercise the real behaviour.
    // -----------------------------------------------------------------------

    WSLC_TEST_METHOD(InstallWithDependenciesNotImplemented)
    {
        VERIFY_ARE_EQUAL(WslcInstallWithDependencies(nullptr, nullptr), E_NOTIMPL);
    }

    // Negative tests: handle lifecycle and invalid state transitions

    WSLC_TEST_METHOD(ReleaseNullSessionHandle)
    {
        VERIFY_ARE_EQUAL(WslcReleaseSession(nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(TerminateNullSessionHandle)
    {
        VERIFY_ARE_EQUAL(WslcTerminateSession(nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(ReleaseNullContainerHandle)
    {
        VERIFY_ARE_EQUAL(WslcReleaseContainer(nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(ReleaseNullProcessHandle)
    {
        VERIFY_ARE_EQUAL(WslcReleaseProcess(nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(CreateContainerWithNullSession)
    {
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));

        WslcContainer container = nullptr;
        VERIFY_ARE_EQUAL(WslcCreateContainer(nullptr, &containerSettings, &container, nullptr), E_POINTER);
    }

    WSLC_TEST_METHOD(StopContainerWithInvalidSignal)
    {
        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        PCSTR argv[] = {"/bin/sleep", "10"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH, nullptr));

        // Wait for the short-lived init process to exit
        UniqueProcess initProcess;
        VERIFY_SUCCEEDED(WslcGetContainerInitProcess(container.get(), &initProcess));
        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(initProcess.get(), &exitEvent));
        VERIFY_ARE_EQUAL(WAIT_OBJECT_0, WaitForSingleObject(exitEvent, 30000));

        // Attempting to exec on a stopped container should fail
        WslcProcessSettings execSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&execSettings));
        PCSTR execArgv[] = {"/bin/echo", "should-fail"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execSettings, execArgv, ARRAYSIZE(execArgv)));

        UniqueProcess execProcess;
        VERIFY_ARE_EQUAL(WslcCreateContainerProcess(container.get(), &execSettings, &execProcess, nullptr), WSLC_E_CONTAINER_NOT_RUNNING);
    }

    WSLC_TEST_METHOD(DuplicateContainerName)
    {
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        PCSTR argv[] = {"/bin/sleep", "10"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsName(&containerSettings, "duplicate-name-test"));

        UniqueContainer container1;
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container1, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container1.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        // Creating a second container with the same name should fail
        UniqueContainer container2;
        VERIFY_ARE_EQUAL(WslcCreateContainer(m_defaultSession, &containerSettings, &container2, nullptr), static_cast<HRESULT>(0x800700b7)); // ERROR_ALREADY_EXISTS
    }

    WSLC_TEST_METHOD(DeleteRunningContainerWithoutForce)
    {
        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        PCSTR argv[] = {"/bin/sleep", "10"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr));

        // Deleting a running container without force flag should fail
        VERIFY_ARE_EQUAL(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr), WSLC_E_CONTAINER_IS_RUNNING);
    }

    WSLC_TEST_METHOD(DeleteNonExistentImage)
    {
        VERIFY_ARE_EQUAL(WslcDeleteSessionImage(m_defaultSession, "nonexistent-image:this-tag-does-not-exist", nullptr), WSLC_E_IMAGE_NOT_FOUND);
    }

    WSLC_TEST_METHOD(PullInvalidImageUri)
    {
        WslcPullImageOptions pullOptions = {};
        pullOptions.uri = "///invalid-registry-url///";
        VERIFY_ARE_EQUAL(WslcPullSessionImage(m_defaultSession, &pullOptions, nullptr), E_INVALIDARG);
    }
};
