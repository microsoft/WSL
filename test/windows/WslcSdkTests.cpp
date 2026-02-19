/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcTests.cpp

Abstract:

    This file contains test cases for the WSLC SDK.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslcsdk.h"

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

namespace {

//
// RAII guards for opaque WSLC handle types.
//

struct WslcSessionGuard
{
    WslcSession session = nullptr;

    WslcSessionGuard() = default;
    WslcSessionGuard(const WslcSessionGuard&) = delete;
    WslcSessionGuard& operator=(const WslcSessionGuard&) = delete;

    ~WslcSessionGuard()
    {
        if (session)
        {
            WslcSessionTerminate(session);
            WslcSessionRelease(session);
        }
    }

    operator WslcSession() const { return session; }
};

struct WslcContainerGuard
{
    WslcContainer container = nullptr;

    WslcContainerGuard() = default;
    WslcContainerGuard(const WslcContainerGuard&) = delete;
    WslcContainerGuard& operator=(const WslcContainerGuard&) = delete;

    ~WslcContainerGuard()
    {
        if (container)
        {
            WslcContainerStop(container, WSLC_SIGNAL_SIGKILL, 30 * 1000);
            WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE);
            WslcContainerRelease(container);
        }
    }

    operator WslcContainer() const { return container; }
};

struct WslcProcessGuard
{
    WslcProcess process = nullptr;

    WslcProcessGuard() = default;
    WslcProcessGuard(const WslcProcessGuard&) = delete;
    WslcProcessGuard& operator=(const WslcProcessGuard&) = delete;

    ~WslcProcessGuard()
    {
        if (process)
        {
            WslcProcessRelease(process);
        }
    }

    operator WslcProcess() const { return process; }
};

// Reads all data from a pipe handle until it closes.
std::string ReadHandleToString(HANDLE handle)
{
    std::string result;
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) && bytesRead > 0)
    {
        result.append(buffer, bytesRead);
    }
    return result;
}

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
    DWORD timeoutMs = 60 * 1000)
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
    WslcContainerGuard container;
    THROW_IF_FAILED(WslcContainerCreate(session, &containerSettings, &container.container, nullptr));
    THROW_IF_FAILED(WslcContainerStart(container));

    // Acquire the init process handle.
    WslcProcessGuard process;
    THROW_IF_FAILED(WslcContainerGetInitProcess(container, &process.process));

    // Borrow the exit-event handle (lifetime tied to the process object; do NOT close it).
    HANDLE exitEvent = nullptr;
    THROW_IF_FAILED(WslcProcessGetExitEvent(process, &exitEvent));

    // Acquire stdout / stderr pipe handles (caller owns these).
    HANDLE rawStdout = nullptr;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process, WSLC_PROCESS_IO_HANDLE_STDOUT, &rawStdout));
    wil::unique_handle ownedStdout(rawStdout);

    HANDLE rawStderr = nullptr;
    THROW_IF_FAILED(WslcProcessGetIOHandles(process, WSLC_PROCESS_IO_HANDLE_STDERR, &rawStderr));
    wil::unique_handle ownedStderr(rawStderr);

    // Read stdout / stderr concurrently so that full pipe buffers do not stall the process.
    auto output = std::make_shared<ContainerOutput>();
    auto readStdout = std::thread([output, ownedStdout = std::move(ownedStdout)]() {
        output->stdoutOutput = ReadHandleToString(ownedStdout.get());
    });
    auto readStderr = std::thread([output, ownedStderr = std::move(ownedStderr)]() {
        output->stderrOutput = ReadHandleToString(ownedStderr.get());
    });
    auto detachThreads = wil::scope_exit([&]() {
        readStdout.detach();
        readStderr.detach();
    });

    // Wait for the process to exit.
    THROW_HR_IF(
        HRESULT_FROM_WIN32(WAIT_TIMEOUT), WaitForSingleObject(exitEvent, timeoutMs) != WAIT_OBJECT_0);

    readStdout.join();
    readStderr.join();
    detachThreads.release();

    return std::move(*output.get());
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

        m_storagePath = std::filesystem::current_path() / "wslc-test-storage";

        // Build session settings using the WSLC SDK.
        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(m_storagePath.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetDisplayName(&sessionSettings, c_testSessionName));
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

        // Create a second session to verify independent sessions work.
        std::filesystem::path extraStorage = std::filesystem::current_path() / "wslc-extra-session-storage";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(extraStorage, ec);
        });

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(extraStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetDisplayName(&sessionSettings, L"wslc-extra-session"));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetCpuCount(&sessionSettings, 2));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetMemory(&sessionSettings, 1024));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));

        WslcVhdRequirements vhdReqs{};
        vhdReqs.sizeInBytes = 1024ull * 1024 * 1024; // 1 GB
        vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
        VERIFY_SUCCEEDED(WslcSessionSettingsSetVHD(&sessionSettings, &vhdReqs));

        WslcSessionGuard session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session.session));
        VERIFY_IS_NOT_NULL(session.session);

        // Null output pointer must fail.
        VERIFY_ARE_EQUAL(WslcSessionCreate(&sessionSettings, nullptr), E_POINTER);

        // Null settings pointer must fail.
        VERIFY_ARE_EQUAL(WslcSessionCreate(nullptr, &session.session), E_POINTER);
    }

    TEST_METHOD(TerminationCallbackViaTerminate)
    {
        WSL2_TEST_ONLY();

        std::promise<WslcSessionTerminationReason> promise;

        auto callback = [](WslcSessionTerminationReason reason, PVOID context) {
            auto* p = static_cast<std::promise<WslcSessionTerminationReason>*>(context);
            p->set_value(reason);
        };

        std::filesystem::path cbStorage = std::filesystem::current_path() / "wslc-termcb-session-storage";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(cbStorage, ec);
        });

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(cbStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetDisplayName(&sessionSettings, L"wslc-termcb-test"));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTerminateCallback(&sessionSettings, callback, &promise));

        WslcSessionGuard session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session.session));

        // Terminating the session should trigger a graceful shutdown and fire the callback.
        WslcSessionTerminate(session.session);
        session.session = nullptr;

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

        std::filesystem::path cbStorage = std::filesystem::current_path() / "wslc-termcb-session-storage";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(cbStorage, ec);
        });

        WslcSessionSettings sessionSettings;
        VERIFY_SUCCEEDED(WslcSessionInitSettings(cbStorage.c_str(), &sessionSettings));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetDisplayName(&sessionSettings, L"wslc-termcb-test"));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTimeout(&sessionSettings, 30 * 1000));
        VERIFY_SUCCEEDED(WslcSessionSettingsSetTerminateCallback(&sessionSettings, callback, &promise));

        WslcSessionGuard session;
        VERIFY_SUCCEEDED(WslcSessionCreate(&sessionSettings, &session.session));

        // Releasing the session should trigger a graceful shutdown and fire the callback.
        WslcSessionRelease(session.session);
        session.session = nullptr;

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
        SKIP_TEST_ARM64();

        // Simple echo — verify stdout is captured correctly.
        {
            auto output = RunContainerAndCapture(m_defaultSession, "debian:latest", {"/bin/echo", "OK"});
            VERIFY_ARE_EQUAL(output.stdoutOutput, "OK\n");
            VERIFY_ARE_EQUAL(output.stderrOutput, "");
        }

        // Verify stdout and stderr are routed independently.
        {
            auto output = RunContainerAndCapture(
                m_defaultSession, "debian:latest", {"/bin/sh", "-c", "echo stdout && echo stderr >&2"});
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
        SKIP_TEST_ARM64();

        // Build a long-running container.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99999"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetName(&containerSettings, "wslc-stop-delete-test"));

        WslcContainerGuard container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container));

        // Acquire and release the init process handle — we won't read its I/O.
        {
            WslcProcessGuard process;
            VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container, &process.process));
        }

        // Stop the container gracefully (after the timeout).
        VERIFY_SUCCEEDED(WslcContainerStop(container, WSLC_SIGNAL_SIGTERM, 10 * 1000));

        // Delete the stopped container.
        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ProcessIOHandles)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        // Verify that stdout and stderr can each be read, and are independent streams.
        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sh", "-c", "printf 'stdout-line\n' ; printf 'stderr-line\n' >&2"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetFlags(&containerSettings, WSLC_CONTAINER_FLAG_NONE));

        WslcContainerGuard container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container));

        WslcProcessGuard process;
        VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container, &process.process));

        HANDLE exitEvent = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetExitEvent(process, &exitEvent));

        HANDLE rawStdout = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process, WSLC_PROCESS_IO_HANDLE_STDOUT, &rawStdout));
        wil::unique_handle ownedStdout(rawStdout);

        HANDLE rawStderr = nullptr;
        VERIFY_SUCCEEDED(WslcProcessGetIOHandles(process, WSLC_PROCESS_IO_HANDLE_STDERR, &rawStderr));
        wil::unique_handle ownedStderr(rawStderr);

        // Verify that each handle can only be acquired once.
        {
            HANDLE duplicate = nullptr;
            VERIFY_ARE_EQUAL(
                WslcProcessGetIOHandles(process, WSLC_PROCESS_IO_HANDLE_STDOUT, &duplicate),
                HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
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
        SKIP_TEST_ARM64();

        WslcContainerGuard container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));

        PCHAR id[WSLC_CONTAINER_ID_LENGTH]{};
        VERIFY_ARE_EQUAL(WslcContainerGetID(container, &id), E_NOTIMPL);

        // Clean up the created container.
        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerGetStateNotImplemented)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        WslcContainerGuard container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));

        WslcContainerState state{};
        VERIFY_ARE_EQUAL(WslcContainerGetState(container, &state), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerInspectNotImplemented)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        WslcContainerGuard container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));

        PCSTR inspectData = nullptr;
        VERIFY_ARE_EQUAL(WslcContainerInspect(container, &inspectData), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerExecNotImplemented)
    {
        WSL2_TEST_ONLY();
        SKIP_TEST_ARM64();

        WslcContainerGuard container;
        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));

        WslcProcess newProcess = nullptr;
        VERIFY_ARE_EQUAL(WslcContainerExec(container, &procSettings, &newProcess), E_NOTIMPL);

        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
    }

    TEST_METHOD(ContainerNetworkingModeNotImplemented)
    {
        WSL2_TEST_ONLY();

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_ARE_EQUAL(
            WslcContainerSettingsSetNetworkingMode(&containerSettings, WSLC_CONTAINER_NETWORKING_MODE_NONE), E_NOTIMPL);
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
        SKIP_TEST_ARM64();

        auto output = RunContainerAndCapture(
            m_defaultSession, "debian:latest", {"/bin/echo", "signal-test"});
        // WslcProcessSignal is tested via a separately created process below.
        // Since we cannot get a valid WslcProcess after RunContainerAndCapture returns,
        // we verify E_NOTIMPL via a dedicated container.

        WslcProcessSettings procSettings;
        VERIFY_SUCCEEDED(WslcProcessInitSettings(&procSettings));
        const char* argv[] = {"/bin/sleep", "99999"};
        VERIFY_SUCCEEDED(WslcProcessSettingsSetCmdLineArgs(&procSettings, argv, ARRAYSIZE(argv)));

        WslcContainerSettings containerSettings;
        VERIFY_SUCCEEDED(WslcContainerInitSettings("debian:latest", &containerSettings));
        VERIFY_SUCCEEDED(WslcContainerSettingsSetInitProcess(&containerSettings, &procSettings));

        WslcContainerGuard container;
        VERIFY_SUCCEEDED(WslcContainerCreate(m_defaultSession, &containerSettings, &container.container, nullptr));
        VERIFY_SUCCEEDED(WslcContainerStart(container));

        WslcProcessGuard process;
        VERIFY_SUCCEEDED(WslcContainerGetInitProcess(container, &process.process));

        VERIFY_ARE_EQUAL(WslcProcessSignal(process, WSLC_SIGNAL_SIGKILL), E_NOTIMPL);

        // Clean up via the container-level stop (which is implemented).
        VERIFY_SUCCEEDED(WslcContainerStop(container, WSLC_SIGNAL_SIGKILL, 30 * 1000));
        VERIFY_SUCCEEDED(WslcContainerDelete(container, WSLC_DELETE_CONTAINER_FLAG_NONE));
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
        VERIFY_ARE_EQUAL(
            WslcProcessSettingsSetIoCallback(&procSettings, WSLC_PROCESS_IO_HANDLE_STDOUT, nullptr, nullptr),
            E_NOTIMPL);
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
