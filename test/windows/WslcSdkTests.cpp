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
    WSL_TEST_CLASS(WslcSdkTests)

    wil::unique_couninitialize_call m_coinit = wil::CoInitializeEx();
    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WslcSession m_defaultSession = nullptr;
    static inline auto c_testSessionName = L"wslc-test";

    void LoadTestImage(std::string_view imageName)
    {
        std::filesystem::path imagePath = GetTestImagePath(imageName);
        wil::unique_hfile imageFile{
            CreateFileW(imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!imageFile);

        LARGE_INTEGER fileSize{};
        THROW_LAST_ERROR_IF(!GetFileSizeEx(imageFile.get(), &fileSize));

        WslcLoadImageOptions options{};
        options.ImageHandle = imageFile.get();
        options.ContentLength = static_cast<uint64_t>(fileSize.QuadPart);

        THROW_IF_FAILED(WslcLoadSessionImage(m_defaultSession, &options, nullptr));
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // Use the same storage path as WSLA runtime tests to reduce pull overhead.
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
        VERIFY_SUCCEEDED(WslcSetSessionSettingsVHD(&sessionSettings, &vhdReqs));

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

    TEST_METHOD(CreateSession)
    {
        WSL2_TEST_ONLY();

        std::filesystem::path extraStorage = m_storagePath / "wslc-extra-session-storage";

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcInitSessionSettings(L"wslc-extra-session", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsCpuCount(&sessionSettings, 2));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsMemory(&sessionSettings, 1024));
        VERIFY_SUCCEEDED(WslcSetSessionSettingsTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 1024ull * 1024 * 1024; // 1 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSetSessionSettingsVHD(&sessionSettings, &vhdReqs));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcCreateSession(&sessionSettings, &session, nullptr));
        VERIFY_IS_NOT_NULL(session.get());

        // Null output pointer must fail.
        VERIFY_ARE_EQUAL(WslcCreateSession(&sessionSettings, nullptr, nullptr), E_POINTER);

        // Null settings pointer must fail.
        UniqueSession session2;
        VERIFY_ARE_EQUAL(WslcCreateSession(nullptr, &session2, nullptr), E_POINTER);
    }

    TEST_METHOD(TerminationCallbackViaTerminate)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(TerminationCallbackViaRelease)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(PullImage)
    {
        WSL2_TEST_ONLY();

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
            VERIFY_ARE_EQUAL(WslcPullSessionImage(m_defaultSession, &opts, &errorMsg), WSLA_E_IMAGE_NOT_FOUND);

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

    TEST_METHOD(ImageList)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(LoadImage)
    {
        WSL2_TEST_ONLY();

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

            WslcLoadImageOptions opts{};
            opts.ImageHandle = imageTarFileHandle.get();
            opts.ContentLength = static_cast<uint64_t>(fileSize.QuadPart);
            VERIFY_SUCCEEDED(WslcLoadSessionImage(m_defaultSession, &opts, nullptr));

            // Verify the loaded image is usable.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:latest", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Negative: null options pointer must fail.
        VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, nullptr, nullptr), E_POINTER);

        // Negative: null ImageHandle must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ContentLength = 1;
            VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, &opts, nullptr), E_INVALIDARG);
        }

        // Negative: INVALID_HANDLE_VALUE must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ImageHandle = INVALID_HANDLE_VALUE;
            opts.ContentLength = 1;
            VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, &opts, nullptr), E_INVALIDARG);
        }

        // Negative: zero ContentLength must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ImageHandle = GetCurrentProcess();
            opts.ContentLength = 0;
            VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, &opts, nullptr), E_INVALIDARG);
        }
    }

    TEST_METHOD(LoadImageNonTar)
    {
        WSL2_TEST_ONLY();
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

            WslcLoadImageOptions opts{};
            opts.ImageHandle = selfFileHandle.get();
            opts.ContentLength = static_cast<uint64_t>(fileSize.QuadPart);
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(WslcLoadSessionImage(m_defaultSession, &opts, &errorMsg), E_FAIL);
            VERIFY_IS_NOT_NULL(errorMsg.get());
        }
    }

    TEST_METHOD(ImageDelete)
    {
        WSL2_TEST_ONLY();

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
        VERIFY_SUCCEEDED(WslcDeleteSessionImage(m_defaultSession, "hello-world:latest", nullptr));

        // Verify the image is no longer present in the list.
        VERIFY_IS_FALSE(checkForImage("hello-world:latest"));

        // Negative: null name must fail.
        VERIFY_ARE_EQUAL(WslcDeleteSessionImage(m_defaultSession, nullptr, nullptr), E_POINTER);
    }

    // -----------------------------------------------------------------------
    // Container lifecycle tests
    // -----------------------------------------------------------------------

    TEST_METHOD(CreateContainer)
    {
        WSL2_TEST_ONLY();

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
            VERIFY_ARE_EQUAL(WslcCreateContainer(m_defaultSession, &containerSettings, &container, &errorMsg), WSLA_E_IMAGE_NOT_FOUND);
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

    TEST_METHOD(ContainerGetID)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));

        // Positive: ID is returned and is the expected length of hex characters.
        CHAR id[WSLC_CONTAINER_ID_LENGTH]{};
        VERIFY_SUCCEEDED(WslcGetContainerID(container.get(), id));
        VERIFY_ARE_EQUAL(strnlen(id, WSLC_CONTAINER_ID_LENGTH), static_cast<size_t>(WSLC_CONTAINER_ID_LENGTH - 1));

        // Negative: null ID buffer must fail.
        VERIFY_ARE_EQUAL(WslcGetContainerID(container.get(), nullptr), E_POINTER);

        VERIFY_SUCCEEDED(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr));
    }

    TEST_METHOD(ContainerGetState)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerStopAndDelete)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessIOHandles)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerNetworkingMode)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerPortMapping)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerVolumeUnit)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerVolumeFunctional)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerExec)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerHostName)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerDomainName)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessEnvVariables)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessSignal)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessGetPid)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessGetExitCode)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessGetState)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ProcessCurrentDirectory)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(GetVersion)
    {
        WSL2_TEST_ONLY();

        // Positive: returns S_OK and fills in a non-zero version.
        {
            WslcVersion version{};
            VERIFY_SUCCEEDED(WslcGetVersion(&version));
            VERIFY_IS_TRUE(version.major > 0 || version.minor > 0 || version.revision > 0);
        }

        // Negative: null pointer must fail.
        VERIFY_ARE_EQUAL(WslcGetVersion(nullptr), E_POINTER);
    }

    // -----------------------------------------------------------------------
    // WslcSetProcessSettingsIOCallback tests
    // -----------------------------------------------------------------------

    TEST_METHOD(ProcessIoCallbackUnit)
    {
        WSL2_TEST_ONLY();

        auto noopCb = [](const BYTE*, uint32_t, PVOID) {};

        // Negative: null processSettings must fail.
        VERIFY_ARE_EQUAL(WslcSetProcessSettingsIOCallback(nullptr, WSLC_PROCESS_IO_HANDLE_STDOUT, noopCb, nullptr), E_POINTER);

        // Negative: STDIN is not a valid output handle.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDIN, noopCb, nullptr), E_INVALIDARG);
        }

        // Negative: out-of-range handle value must fail.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsIOCallback(&procSettings, static_cast<WslcProcessIOHandle>(99), noopCb, nullptr), E_INVALIDARG);
        }

        // Negative: null callback with non-null context must fail.
        {
            int ctx = 0;
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, nullptr, &ctx), E_INVALIDARG);
        }

        // Positive: clear STDOUT (null callback, null context) must succeed.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, nullptr, nullptr));
        }

        // Positive: clear STDERR (null callback, null context) must succeed.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDERR, nullptr, nullptr));
        }

        // Positive: set a valid STDOUT callback must succeed.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, noopCb, nullptr));
        }

        // Positive: set a valid STDERR callback must succeed.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDERR, noopCb, nullptr));
        }

        // Negative: StartContainer without ATTACH fails when callbacks are registered.
        // The ATTACH flag is required so the IOCallback can claim the init process pipe handles.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
            const char* argv[] = {"/bin/sleep", "1"};
            VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
            VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, noopCb, nullptr));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcSetContainerSettingsInitProcess(&containerSettings, &procSettings));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_ARE_EQUAL(WslcStartContainer(container.get(), WSLC_CONTAINER_START_FLAG_NONE, nullptr), E_INVALIDARG);
        }
    }

    TEST_METHOD(ProcessIoCallbackInitProcess)
    {
        WSL2_TEST_ONLY();

        std::string stdoutData;
        std::string stderrData;

        auto appendToContextString = [](const BYTE* data, uint32_t size, PVOID ctx) {
            static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "echo STDOUT && echo STDERR >&2"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, appendToContextString, &stdoutData));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDERR, appendToContextString, &stderrData));

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

        VERIFY_ARE_EQUAL(stdoutData, "STDOUT\n");
        VERIFY_ARE_EQUAL(stderrData, "STDERR\n");
    }

    TEST_METHOD(ProcessIoCallbackExecProcess)
    {
        WSL2_TEST_ONLY();

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

        std::string stdoutData;
        std::string stderrData;

        auto stdoutCb = [](const BYTE* data, uint32_t size, PVOID ctx) {
            static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
        };
        auto stderrCb = [](const BYTE* data, uint32_t size, PVOID ctx) {
            static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings execProcSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&execProcSettings));
        const char* execArgv[] = {"/bin/sh", "-c", "echo EXEC_OUT && echo EXEC_ERR >&2"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&execProcSettings, execArgv, ARRAYSIZE(execArgv)));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&execProcSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, stdoutCb, &stdoutData));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&execProcSettings, WSLC_PROCESS_IO_HANDLE_STDERR, stderrCb, &stderrData));

        UniqueProcess execProcess;
        VERIFY_SUCCEEDED(WslcCreateContainerProcess(container.get(), &execProcSettings, &execProcess, nullptr));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcGetProcessExitEvent(execProcess.get(), &exitEvent));
        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        // Releasing the exec process handle destroys WslcProcessImpl and joins its IOCallback
        // thread, ensuring all bytes are delivered before assertions.
        execProcess.reset();

        VERIFY_ARE_EQUAL(stdoutData, "EXEC_OUT\n");
        VERIFY_ARE_EQUAL(stderrData, "EXEC_ERR\n");
    }

    TEST_METHOD(ProcessIoCallbackHandleExclusion)
    {
        WSL2_TEST_ONLY();

        // Register a stdout callback only — no stderr callback.
        // The IOCallback object will claim stdout's pipe handle via GetStdHandle (ownership
        // transfer); calling WslcGetProcessIOHandle for stdout afterwards must therefore fail
        // with ERROR_INVALID_STATE.  The stderr handle (no callback) must remain obtainable.
        auto noopCb = [](const BYTE*, uint32_t, PVOID) {};

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, noopCb, nullptr));

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

    TEST_METHOD(ProcessIoCallbackLargeOutput)
    {
        WSL2_TEST_ONLY();

        // Generate ~1 MiB of stdout via: dd if=/dev/zero bs=1024 count=1024 | base64
        // 1,048,576 zero bytes → base64 output is 1,398,104 bytes (ceil(1048576/3)*4 + newlines).
        // We verify the accumulated size is at least 1,398,000 bytes to allow minor variance.
        static constexpr size_t c_expectedMinBytes = 1'398'000;

        std::string stdoutData;
        stdoutData.reserve(c_expectedMinBytes + 4096);

        auto stdoutCb = [](const BYTE* data, uint32_t size, PVOID ctx) {
            static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), size);
        };

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcInitProcessSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "dd if=/dev/zero bs=1024 count=1024 2>/dev/null | base64"};
        VERIFY_SUCCEEDED(WslcSetProcessSettingsCmdLine(&procSettings, argv, ARRAYSIZE(argv)));
        VERIFY_SUCCEEDED(WslcSetProcessSettingsIOCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, stdoutCb, &stdoutData));

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

        VERIFY_IS_TRUE(stdoutData.size() >= c_expectedMinBytes);
    }

    // -----------------------------------------------------------------------
    // Stub tests for unimplemented (E_NOTIMPL) functions.
    // Each of these confirms the current state of the SDK; once the underlying
    // function is implemented the assertion below will catch it and the test
    // should be updated to exercise the real behaviour.
    // -----------------------------------------------------------------------

    TEST_METHOD(CanRunNotImplemented)
    {
        WSL2_TEST_ONLY();

        BOOL canRun = FALSE;
        WslcComponentFlags missing{};
        VERIFY_ARE_EQUAL(WslcCanRun(&canRun, &missing), E_NOTIMPL);
    }

    TEST_METHOD(ImageImportNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcImportImageOptions opts{};
        opts.imagePath = L"dummy.tar";
        VERIFY_ARE_EQUAL(WslcImportSessionImage(m_defaultSession, &opts, nullptr), E_NOTIMPL);
    }

    TEST_METHOD(ContainerInspectNotImplemented)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcInitContainerSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcCreateContainer(m_defaultSession, &containerSettings, &container, nullptr));

        PCSTR inspectData = nullptr;
        VERIFY_ARE_EQUAL(WslcInspectContainer(container.get(), &inspectData), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcDeleteContainer(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr));
    }

    TEST_METHOD(SessionCreateVhdNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcVhdRequirements vhd{};
        vhd.sizeInBytes = 1024ull * 1024 * 1024;
        vhd.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_ARE_EQUAL(WslcCreateSessionVhd(m_defaultSession, &vhd, nullptr), E_NOTIMPL);
    }

    TEST_METHOD(InstallWithDependenciesNotImplemented)
    {
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(WslcInstallWithDependencies(nullptr, nullptr), E_NOTIMPL);
    }
};
