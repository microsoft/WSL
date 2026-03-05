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
        WslcSessionTerminate(session);
        WslcSessionRelease(session);
    }
}

using UniqueSession = wil::unique_any<WslcSession, decltype(CloseSession), CloseSession>;

void CloseContainer(WslcContainer container)
{
    if (container)
    {
        WslcContainerStop(container, WSLC_SIGNAL_SIGKILL, 30);
        WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE);
        WslcContainerRelease(container);
    }
}

using UniqueContainer = wil::unique_any<WslcContainer, decltype(CloseContainer), CloseContainer>;

void CloseProcess(WslcProcess process)
{
    if (process)
    {
        WslcProcessRelease(process);
    }
}

using UniqueProcess = wil::unique_any<WslcProcess, decltype(CloseProcess), CloseProcess>;

struct ContainerOutput
{
    std::string stdoutOutput;
    std::string stderrOutput;
};

//
// Runs a container with the given argv, waits up to timeoutMs for it to exit,
// and returns the captured stdout / stderr output.
//
ContainerOutput RunContainerAndCapture(
    WslcSession session,
    const char* image,
    const std::vector<const char*>& argv,
    WslcContainerFlags flags = WSLC_CONTAINER_FLAG_NONE,
    const char* name = nullptr,
    std::chrono::milliseconds timeout = 60s,
    std::optional<WslcContainerNetworkingMode> networkingMode = std::nullopt)
{
    // Build process settings.
    WslcProcessSettings procSettings;
    THROW_IF_FAILED(WslcProcessInitSettings(&procSettings));
    if (!argv.empty())
    {
        THROW_IF_FAILED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv.data(), argv.size()));
    }

    // Build container settings.
    WslcContainerSettings containerSettings;
    THROW_IF_FAILED(WslcContainerInitSettings(image, &containerSettings));
    THROW_IF_FAILED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
    THROW_IF_FAILED(WslcContainerSettingsSetFlags(&containerSettings, flags));
    if (name)
    {
        THROW_IF_FAILED(WslcContainerSettingsSetName(&containerSettings, name));
    }
    if (networkingMode.has_value())
    {
        THROW_IF_FAILED(WslcContainerSettingsSetNetworkingMode(&containerSettings, *networkingMode));
    }

    // Create and start the container.
    UniqueContainer container;
    THROW_IF_FAILED(WslcContainerCreate(session, &containerSettings, &container, nullptr));
    THROW_IF_FAILED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH));

    // Acquire the init process handle.
    UniqueProcess process;
    THROW_IF_FAILED(WslcContainerGetInitProcess(container.get(), &process));

    // Borrow the exit-event handle (lifetime tied to the process object; do NOT close it).
    HANDLE exitEvent = nullptr;
    THROW_IF_FAILED(WslcProcessGetExitEvent(process.get(), &exitEvent));

    // Acquire stdout / stderr pipe handles (caller owns these).
    wil::unique_handle ownedStdout;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

    wil::unique_handle ownedStderr;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &ownedStderr));

    // Read stdout / stderr concurrently so that full pipe buffers do not stall the process.
    ContainerOutput output;
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

        THROW_IF_FAILED(WslcSessionImageLoad(m_defaultSession, &options));
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // Use the same storage path as WSLA runtime tests to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";

        // Build session settings using the WSLC SDK.
        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(c_testSessionName, m_storagePath.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetCpuCount(&sessionSettings, 4));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetMemory(&sessionSettings, 2048));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 4096ull * 1024 * 1024; // 4 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSessionSettingsSetVHD(&sessionSettings, &vhdReqs));

        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &m_defaultSession));

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
            WslcSessionTerminate(m_defaultSession);
            WslcSessionRelease(m_defaultSession);
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
        VERIFY_SUCCEEDED(WslcSessionInitSettings(L"wslc-extra-session", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetCpuCount(&sessionSettings, 2));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetMemory(&sessionSettings, 1024));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 1024ull * 1024 * 1024; // 1 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSessionSettingsSetVHD(&sessionSettings, &vhdReqs));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session));
        VERIFY_IS_NOT_NULL(session.get());

        // Null output pointer must fail.
        VERIFY_ARE_EQUAL(WslcSessionCreate(&sessionSettings, nullptr), E_POINTER);

        // Null settings pointer must fail.
        UniqueSession session2;
        VERIFY_ARE_EQUAL(WslcSessionCreate(nullptr, &session2), E_POINTER);
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
        VERIFY_SUCCEEDED(WslcSessionInitSettings(L"wslc-termcb-term-test", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTerminateCallback(&sessionSettings, callback, &promise));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session));

        // Terminating the session should trigger a graceful shutdown and fire the callback.
        VERIFY_SUCCEEDED(WslcSessionTerminate(session.get()));

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
        VERIFY_SUCCEEDED(WslcSessionInitSettings(L"wslc-termcb-release-test", extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTerminateCallback(&sessionSettings, callback, &promise));

        UniqueSession session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session));

        // Releasing the session should trigger a graceful shutdown and fire the callback.
        VERIFY_SUCCEEDED(WslcSessionRelease(session.get()));
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
            HRESULT pullResult = WslcSessionImagePull(m_defaultSession, &opts, &errorMsg);

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
            VERIFY_FAILED(WslcSessionImagePull(m_defaultSession, &opts, &errorMsg));

            // An error message should be present.
            VERIFY_IS_NOT_NULL(errorMsg.get());
        }

        // Negative: null options pointer must fail.
        {
            wil::unique_cotaskmem_string errorMsg;
            VERIFY_ARE_EQUAL(WslcSessionImagePull(m_defaultSession, nullptr, &errorMsg), E_POINTER);
        }

        // Negative: null URI inside options must fail.
        {
            WslcPullImageOptions opts{};
            opts.uri = nullptr;
            VERIFY_ARE_EQUAL(WslcSessionImagePull(m_defaultSession, &opts, nullptr), E_INVALIDARG);
        }
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
            VERIFY_SUCCEEDED(WslcContainerInitSettings("invalid-image:notfound", &containerSettings));

            WslcContainer container = nullptr;
            PWSTR rawMsg = nullptr;
            VERIFY_FAILED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, &rawMsg));
            wil::unique_cotaskmem_string errorMsg{rawMsg};
            VERIFY_IS_NULL(container);
        }

        // Verify that a null image name is rejected.
        {
            WslcContainerSettings containerSettings;
            VERIFY_ARE_EQUAL(WslcContainerInitSettings(nullptr, &containerSettings), E_POINTER);
        }

        // Verify that a null settings pointer is rejected.
        {
            VERIFY_ARE_EQUAL(WslcContainerInitSettings("debian:latest", nullptr), E_POINTER);
        }

        // Verify that a null container output pointer is rejected.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcContainerCreate(m_defaultSession, &containerSettings, nullptr, nullptr), E_POINTER);
        }
    }

    TEST_METHOD(ContainerStopAndDelete)
    {
        WSL2_TEST_ONLY();

        // Build a long-running container.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "999"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetName(&containerSettings, "wslc-stop-delete-test"));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_NONE));

        // Acquire and release the init process handle — we won't read its I/O.
        {
            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));
        }

        // Stop the container gracefully (after the timeout).
        VERIFY_SUCCEEDED(WslcContainerStop(container.get(), WSLC_SIGNAL_SIGKILL, 10));

        // Delete the stopped container.
        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ProcessIOHandles)
    {
        WSL2_TEST_ONLY();

        // Verify that stdout and stderr can each be read, and are independent streams.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "printf 'stdout-line\n' ; printf 'stderr-line\n' >&2"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetFlags(&containerSettings, WSLC_CONTAINER_FLAG_NONE));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetExitEvent(process.get(), &exitEvent));

        HANDLE rawStdout = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &rawStdout));
        wil::unique_handle ownedStdout(rawStdout);

        HANDLE rawStderr = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &rawStderr));
        wil::unique_handle ownedStderr(rawStderr);

        // Verify that each handle can only be acquired once.
        {
            HANDLE duplicate = nullptr;
            VERIFY_ARE_EQUAL(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &duplicate), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 60 * 1000), WAIT_OBJECT_0);
    }

    TEST_METHOD(LoadImage)
    {
        WSL2_TEST_ONLY();

        // Positive: load a saved image tar and verify the image can be run.
        {
            // TODO: Remove the image before attempting to load it

            std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

            WslcLoadImageOptions opts{};
            opts.ImageHandle = imageTarFileHandle.get();
            opts.ContentLength = static_cast<uint64_t>(fileSize.QuadPart);
            VERIFY_SUCCEEDED(WslcSessionImageLoad(m_defaultSession, &opts));

            // Verify the loaded image is usable.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:latest", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Negative: null options pointer must fail.
        VERIFY_ARE_EQUAL(WslcSessionImageLoad(m_defaultSession, nullptr), E_POINTER);

        // Negative: null ImageHandle must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ContentLength = 1;
            VERIFY_ARE_EQUAL(WslcSessionImageLoad(m_defaultSession, &opts), E_INVALIDARG);
        }

        // Negative: INVALID_HANDLE_VALUE must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ImageHandle = INVALID_HANDLE_VALUE;
            opts.ContentLength = 1;
            VERIFY_ARE_EQUAL(WslcSessionImageLoad(m_defaultSession, &opts), E_INVALIDARG);
        }

        // Negative: zero ContentLength must fail.
        {
            WslcLoadImageOptions opts{};
            opts.ImageHandle = GetCurrentProcess();
            opts.ContentLength = 0;
            VERIFY_ARE_EQUAL(WslcSessionImageLoad(m_defaultSession, &opts), E_INVALIDARG);
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
            VERIFY_FAILED(WslcSessionImageLoad(m_defaultSession, &opts));
        }
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
            VERIFY_IS_TRUE(output.stdoutOutput.find("HAS_ETH0") != std::string::npos);
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
            VERIFY_IS_TRUE(output.stdoutOutput.find("NO_ETH0") != std::string::npos);
        }

        // Invalid networking mode must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetNetworkingMode(&containerSettings, static_cast<WslcContainerNetworkingMode>(99)), E_INVALIDARG);
        }
    }

    TEST_METHOD(ContainerPortMapping)
    {
        WSL2_TEST_ONLY();

        // Negative: null mappings with nonzero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetPortMapping(&containerSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            WslcContainerPortMapping portMappings[1] = {};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetPortMapping(&containerSettings, portMappings, 0), E_INVALIDARG);
        }

        // Positive: null mappings with zero count must succeed (clears the mapping).
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetPortMapping(&containerSettings, nullptr, 0));
        }

        // Negative: port mappings with NONE networking must fail at container creation.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_NONE));

            WslcContainerPortMapping mapping{};
            mapping.windowsPort = 12342;
            mapping.containerPort = 8000;
            mapping.protocol = WSLC_PORT_PROTOCOL_TCP;
            VERIFY_SUCCEEDED(WslcContainerSettingsSetPortMapping(&containerSettings, &mapping, 1));

            WslcContainer rawContainer = nullptr;
            VERIFY_FAILED(WslcContainerCreate(m_defaultSession, &containerSettings, &rawContainer, nullptr));
            VERIFY_IS_NULL(rawContainer);
        }

        // Functional: create a container with BRIDGED networking and a port mapping;
        // verify that a TCP connection from the host reaches the container.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            const char* argv[] = {"python3", "-m", "http.server", "8000"};
            VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));
            const char* env[] = {"PYTHONUNBUFFERED=1"};
            VERIFY_SUCCEEDED(WslcProcessSettingsSetEnvVariables(&procSettings, env, ARRAYSIZE(env)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("python:3.12-alpine", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_BRIDGED));

            WslcContainerPortMapping mapping{};
            mapping.windowsPort = 12341;
            mapping.containerPort = 8000;
            mapping.protocol = WSLC_PORT_PROTOCOL_TCP;
            VERIFY_SUCCEEDED(WslcContainerSettingsSetPortMapping(&containerSettings, &mapping, 1));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH));

            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));

            wil::unique_handle ownedStdout;
            VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

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
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, 0), E_INVALIDARG);
        }

        // Negative: null paths must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {nullptr, "/mnt/path"};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), nullptr};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        // Relative paths must fail.
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            WslcContainerVolume containerVolumes[1] = {L"relative", "/mnt/path"};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), "./mnt/path"};
            VERIFY_ARE_EQUAL(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)), E_INVALIDARG);
        }

        // Positive: null volumes with zero count must succeed (clears volumes).
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetVolumes(&containerSettings, nullptr, 0));
        }

        // Absolute paths should succeed
        {
            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            auto currentDirectory = std::filesystem::current_path();
            WslcContainerVolume containerVolumes[1] = {currentDirectory.c_str(), "/mnt/path"};
            VERIFY_SUCCEEDED(WslcContainerSettingsSetVolumes(&containerSettings, containerVolumes, ARRAYSIZE(containerVolumes)));
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
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", script};
            VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetVolumes(&containerSettings, volumes, 2));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH));

            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));

            HANDLE exitEvent = nullptr;
            VERIFY_SUCCEEDED(WslcProcessGetExitEvent(process.get(), &exitEvent));

            wil::unique_handle ownedStdout;
            VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

            wil::unique_handle ownedStderr;
            VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &ownedStderr));

            ContainerOutput output;
            wsl::windows::common::relay::MultiHandleWait io;
            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
                std::move(ownedStdout), [&](const auto& buf) { output.stdoutOutput.append(buf.data(), buf.size()); }));
            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
                std::move(ownedStderr), [&](const auto& buf) { output.stderrOutput.append(buf.data(), buf.size()); }));
            io.Run(60s);

            VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 60 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

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
            VERIFY_IS_TRUE(writtenContent.find("container-write") != std::string::npos);
        }
    }

    TEST_METHOD(ProcessEnvVariables)
    {
        WSL2_TEST_ONLY();

        // Negative: null pointer with nonzero count must fail.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            VERIFY_ARE_EQUAL(WslcProcessSettingsSetEnvVariables(&procSettings, nullptr, 1), E_INVALIDARG);
        }

        // Negative: non-null pointer with zero count must fail.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            const char* envVars[] = {"FOO=bar"};
            VERIFY_ARE_EQUAL(WslcProcessSettingsSetEnvVariables(&procSettings, envVars, 0), E_INVALIDARG);
        }

        // Positive: null pointer with zero count must succeed (clears env vars).
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            VERIFY_SUCCEEDED(WslcProcessSettingsSetEnvVariables(&procSettings, nullptr, 0));
        }

        // Functional: set an env var and verify it is visible inside the container.
        {
            WslcProcessSettings procSettings;
            VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
            const char* argv[] = {"/bin/sh", "-c", "echo $MY_TEST_VAR"};
            VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));
            const char* envVars[] = {"MY_TEST_VAR=hello-from-test"};
            VERIFY_SUCCEEDED(WslcProcessSettingsSetEnvVariables(&procSettings, envVars, ARRAYSIZE(envVars)));

            WslcContainerSettings containerSettings;
            VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
            VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));

            UniqueContainer container;
            VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
            VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_ATTACH));

            UniqueProcess process;
            VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));

            HANDLE exitEvent = nullptr;
            VERIFY_SUCCEEDED(WslcProcessGetExitEvent(process.get(), &exitEvent));

            wil::unique_handle ownedStdout;
            VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &ownedStdout));

            wil::unique_handle ownedStderr;
            VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &ownedStderr));

            std::string stdoutOutput;
            wsl::windows::common::relay::MultiHandleWait io;
            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(
                std::move(ownedStdout), [&](const auto& buf) { stdoutOutput.append(buf.data(), buf.size()); }));
            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(std::move(ownedStderr), [&](const auto& buf) {}));
            io.Run(60s);

            VERIFY_ARE_EQUAL(WaitForSingleObject(exitEvent, 10 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));
            VERIFY_IS_TRUE(stdoutOutput.find("hello-from-test") != std::string::npos);
        }
    }

    // -----------------------------------------------------------------------
    // Stub tests for unimplemented (E_NOTIMPL) functions.
    // Each of these confirms the current state of the SDK; once the underlying
    // function is implemented the assertion below will catch it and the test
    // should be updated to exercise the real behaviour.
    // -----------------------------------------------------------------------

    TEST_METHOD(GetVersionNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcVersion version{};
        VERIFY_ARE_EQUAL(WslcGetVersion(&version), E_NOTIMPL);
    }

    TEST_METHOD(CanRunNotImplemented)
    {
        WSL2_TEST_ONLY();

        BOOL canRun = FALSE;
        WslcComponentFlags missing{};
        VERIFY_ARE_EQUAL(WslcCanRun(&canRun, &missing), E_NOTIMPL);
    }

    TEST_METHOD(ImageListNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcImageInfo* images = nullptr;
        uint32_t count = 0;
        VERIFY_ARE_EQUAL(WslcSessionImageList(m_defaultSession, &images, &count), E_NOTIMPL);
    }

    TEST_METHOD(ImageDeleteNotImplemented)
    {
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(WslcSessionImageDelete(m_defaultSession, "debian:latest"), E_NOTIMPL);
    }

    TEST_METHOD(ImageImportNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcImportImageOptions opts{};
        opts.imagePath = L"dummy.tar";
        VERIFY_ARE_EQUAL(WslcSessionImageImport(m_defaultSession, &opts), E_NOTIMPL);
    }

    TEST_METHOD(ContainerGetIDNotImplemented)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));

        CHAR id[WSLC_CONTAINER_ID_LENGTH]{};
        VERIFY_ARE_EQUAL(WslcContainerGetID(container.get(), id), E_NOTIMPL);

        // Clean up the created container.
        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerGetStateNotImplemented)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));

        WslcContainerState state{};
        VERIFY_ARE_EQUAL(WslcContainerGetState(container.get(), &state), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerInspectNotImplemented)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));

        PCSTR inspectData = nullptr;
        VERIFY_ARE_EQUAL(WslcContainerInspect(container.get(), &inspectData), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerExecNotImplemented)
    {
        WSL2_TEST_ONLY();

        UniqueContainer container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));

        WslcProcess newProcess = nullptr;
        VERIFY_ARE_EQUAL(WslcContainerExec(container.get(), &procSettings, &newProcess), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerHostNameNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_ARE_EQUAL(WslcContainerSettingsSetHostName(&containerSettings, "my-host"), E_NOTIMPL);
    }

    TEST_METHOD(ContainerDomainNameNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_ARE_EQUAL(WslcContainerSettingsSetDomainName(&containerSettings, "my-domain"), E_NOTIMPL);
    }

    TEST_METHOD(ProcessSignalNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));

        UniqueContainer container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container.get(), WSLC_CONTAINER_START_FLAG_NONE));

        UniqueProcess process;
        VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container.get(), &process));

        VERIFY_ARE_EQUAL(WslcProcessSignal(process.get(), WSLC_SIGNAL_SIGKILL), E_NOTIMPL);
    }

    TEST_METHOD(ProcessGetPidNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcess process = nullptr;
        uint32_t pid = 0;
        VERIFY_ARE_EQUAL(WslcProcessGetPid(process, &pid), E_NOTIMPL);
    }

    TEST_METHOD(ProcessGetExitCodeNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcess process = nullptr;
        INT32 exitCode = 0;
        VERIFY_ARE_EQUAL(WslcProcessGetExitCode(process, &exitCode), E_NOTIMPL);
    }

    TEST_METHOD(ProcessGetStateNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcess process = nullptr;
        WslcProcessState state{};
        VERIFY_ARE_EQUAL(WslcProcessGetState(process, &state), E_NOTIMPL);
    }

    TEST_METHOD(ProcessCurrentDirectoryNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        VERIFY_ARE_EQUAL(WslcProcessSettingsSetCurrentDirectory(&procSettings, "/tmp"), E_NOTIMPL);
    }

    TEST_METHOD(ProcessIoCallbackNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        VERIFY_ARE_EQUAL(WslcProcessSettingsSetIoCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, nullptr, nullptr), E_NOTIMPL);
    }

    TEST_METHOD(SessionCreateVhdNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcVhdRequirements vhd{};
        vhd.sizeInBytes = 1024ull * 1024 * 1024;
        vhd.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_ARE_EQUAL(WslcSessionCreateVhd(m_defaultSession, &vhd), E_NOTIMPL);
    }

    TEST_METHOD(InstallWithDependenciesNotImplemented)
    {
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(WslcInstallWithDependencies(nullptr, nullptr), E_NOTIMPL);
    }
};
