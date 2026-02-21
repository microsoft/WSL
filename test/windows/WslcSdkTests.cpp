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
    std::chrono::milliseconds timeout = 60s)
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
    HANDLE rawStdout = nullptr;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDOUT, &rawStdout));
    wil::unique_handle ownedStdout(rawStdout);

    HANDLE rawStderr = nullptr;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process.get(), WSLC_PROCESS_IO_HANDLE_STDERR, &rawStderr));
    wil::unique_handle ownedStderr(rawStderr);

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

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // Use the same storage path as WSLA runtime tests to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";

        // Build session settings using the WSLC SDK.
        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(c_testSessionName, m_storagePath.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetCpuCount(&sessionSettings, 4));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetMemory(&sessionSettings, 2024));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 4096ull * 1024 * 1024; // 4 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSessionSettingsSetVHD(&sessionSettings, &vhdReqs));

        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &m_defaultSession));

        // Pull images required by the tests (no-op if already present).
        for (const char* image : {"debian:latest", "hello-world:linux"})
        {
            WslcPullImageOptions pullOptions{};
            pullOptions.uri = image;
            PWSTR rawErrorMsg = nullptr;
            const auto hr = WslcSessionImagePull(m_defaultSession, &pullOptions, &rawErrorMsg);
            wil::unique_cotaskmem_string errorMsg{rawErrorMsg};
            if (FAILED(hr))
            {
                LogError("Failed to pull image '%hs': 0x%08x, %ls", image, hr, errorMsg ? errorMsg.get() : L"(no message)");
                return false;
            }
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
            PWSTR rawMsg = nullptr;
            VERIFY_SUCCEEDED(WslcSessionImagePull(m_defaultSession, &opts, &rawMsg));
            wil::unique_cotaskmem_string errorMsg{rawMsg};

            // Verify the image is usable by running a container from it.
            auto output = RunContainerAndCapture(m_defaultSession, "hello-world:linux", {});
            VERIFY_IS_TRUE(output.stdoutOutput.find("Hello from Docker!") != std::string::npos);
        }

        // Negative: pull an image that does not exist.
        {
            WslcPullImageOptions opts{};
            opts.uri = "does-not:exist";
            PWSTR rawMsg = nullptr;
            VERIFY_FAILED(WslcSessionImagePull(m_defaultSession, &opts, &rawMsg));
            wil::unique_cotaskmem_string errorMsg{rawMsg};

            // An error message should be present.
            VERIFY_IS_NOT_NULL(errorMsg.get());
        }

        // Negative: null options pointer must fail.
        {
            PWSTR rawMsg = nullptr;
            VERIFY_ARE_EQUAL(WslcSessionImagePull(m_defaultSession, nullptr, &rawMsg), E_POINTER);
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
        VERIFY_SUCCEEDED(WslcContainerStop(container.get(), WSLC_SIGNAL_SIGTERM, 10));

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

    TEST_METHOD(ImageLoadNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcLoadImageOptions opts{};
        VERIFY_ARE_EQUAL(WslcSessionImageLoad(m_defaultSession, &opts), E_NOTIMPL);
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

    TEST_METHOD(ContainerNetworkingModeNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_ARE_EQUAL(WslcContainerSettingsSetNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_NONE), E_NOTIMPL);
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

    TEST_METHOD(ContainerPortMappingNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));

        WslcContainerPortMapping mapping{};
        mapping.windowsPort = 8080;
        mapping.containerPort = 80;
        mapping.protocol = WSLC_PORT_PROTOCOL_TCP;
        VERIFY_ARE_EQUAL(WslcContainerSettingsSetPortMapping(&containerSettings, &mapping, 1), E_NOTIMPL);
    }

    TEST_METHOD(ContainerVolumeNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));

        WslcContainerVolume volume{};
        volume.windowsPath = L"C:\\temp";
        volume.containerPath = "/mnt/tmp";
        volume.readOnly = FALSE;
        VERIFY_ARE_EQUAL(WslcContainerSettingsAddVolume(&containerSettings, &volume, 1), E_NOTIMPL);
    }

    TEST_METHOD(ProcessSignalNotImplemented)
    {
        WSL2_TEST_ONLY();

        auto output = RunContainerAndCapture(m_defaultSession, "debian:latest", {"/bin/echo", "signal-test"});
        // WslcProcessSignal is tested via a separately created process below.
        // Since we cannot get a valid WslcProcess after RunContainerAndCapture returns,
        // we verify E_NOTIMPL via a dedicated container.

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "999"};
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

        // Clean up via the container-level stop (which is implemented).
        VERIFY_SUCCEEDED(WslcContainerStop(container.get(), WSLC_SIGNAL_SIGKILL, 30));
        VERIFY_SUCCEEDED(WslcContainerDelete(container.get(), WSLC_DELETE_CONTAINER_FLAG_NONE));
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

    TEST_METHOD(ProcessEnvVariablesNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* envVars[] = {"FOO=bar"};
        VERIFY_ARE_EQUAL(WslcProcessSettingsSetEnvVariables(&procSettings, envVars, ARRAYSIZE(envVars)), E_NOTIMPL);
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
