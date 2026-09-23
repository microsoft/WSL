/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCIdleTerminationTests.cpp

Abstract:

    This file contains test cases for WSLC session idle termination.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCIdleTerminationTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCIdleTerminationTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(ElevatedTokenCanOpenNonElevatedHandles)
    {
        wil::com_ptr<IWSLCSession> nonElevatedSession;

        {
            auto nonElevatedToken = GetNonElevatedToken(TokenImpersonation);
            auto revert = wil::impersonate_token(nonElevatedToken.get());

            nonElevatedSession = CreateSession(GetDefaultSessionSettings(L"non-elevated-session"), WSLCSessionFlagsNone);
            LoadTestImage(*nonElevatedSession, "debian:latest");

            WSLCContainerLauncher launcher("debian:latest", "test-non-elevated-handles-1", {"echo", "OK"});
            auto container = launcher.Launch(*nonElevatedSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OK\n"}});
        }

        WSLCContainerLauncher launcher("debian:latest", "test-non-elevated-handles-2", {"echo", "OK"});
        auto container = launcher.Launch(*nonElevatedSession);
        auto initProcess = container.GetInitProcess();

        ValidateProcessOutput(initProcess, {{1, "OK\n"}});
    }

    // Kills all VMs matching the given owner name via hcsdiag.
    static void KillVmByOwner(const std::wstring& owner)
    {
        bool found = false;
        for (const auto& vm : ListVms())
        {
            if (vm.Owner == owner)
            {
                found = true;
                VERIFY_ARE_EQUAL(wsl::windows::common::SubProcess(nullptr, std::format(L"hcsdiag.exe kill {}", vm.Id).c_str()).Run(10000), 0u);
            }
        }

        VERIFY_IS_TRUE(found, std::format(L"VM with owner '{}' not found", owner).c_str());
    }

    // Waits for a session to report the terminated state.
    static void WaitForSessionTermination(IWSLCSession* session)
    {
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                WSLCSessionState state{};
                THROW_IF_FAILED(session->GetState(&state));
                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_RETRY), state != WSLCSessionStateTerminated);
            },
            std::chrono::seconds{1},
            std::chrono::minutes{2});
    }

    // Returns true if any running VM is owned by the given name.
    static bool IsVmRunning(const std::wstring& owner)
    {
        return std::ranges::any_of(ListVms(), [&](const auto& vm) { return vm.Owner == owner; });
    }

    WSLC_TEST_METHOD(VmKillTerminatesSession)
    {
        constexpr auto c_sessionName = L"wslc-vm-kill-test";
        auto settings = GetDefaultSessionSettings(c_sessionName);
        auto session = CreateSession(settings);

        // Session creation is lazy, so start the VM by launching a process before killing it.
        WSLCProcessLauncher launcher("/bin/sleep", {"/bin/sleep", "60"});
        auto process = launcher.Launch(*session);

        KillVmByOwner(c_sessionName);

        WaitForSessionTermination(session.get());
        VERIFY_IS_FALSE(IsVmRunning(c_sessionName));
    }

    WSLC_TEST_METHOD(VmKillFailsInFlightOperations)
    {
        constexpr auto c_sessionName = L"wslc-vm-kill-inflight-test";
        auto settings = GetDefaultSessionSettings(c_sessionName);
        auto session = CreateSession(settings);

        WSLCProcessLauncher launcher("/bin/sleep", {"/bin/sleep", "60"});
        auto process = launcher.Launch(*session);

        KillVmByOwner(c_sessionName);

        // The process and session should both terminate (not hang).
        WaitForSessionTermination(session.get());
        VERIFY_IS_TRUE(process.GetExitEvent().wait(10000));

        VERIFY_IS_FALSE(IsVmRunning(c_sessionName));
    }

    // TriggerIdleTermination runs the idle-teardown path synchronously and reports whether the VM
    // was already idle. Validates the idle -> running -> forced-idle -> running lifecycle.
    WSLC_TEST_METHOD(TriggerIdleTerminationRestartsVm)
    {
        constexpr auto c_sessionName = L"wslc-idle-trigger-test";

        // Idle termination is only permitted for storage-backed sessions (tmpfs state is
        // unrecoverable), so this lifecycle test uses a dedicated storage directory.
        const auto storageDir = std::filesystem::current_path() / "test-storage-idle-restart";
        std::error_code storageError;
        std::filesystem::remove_all(storageDir, storageError);
        std::filesystem::create_directories(storageDir);
        auto storageCleanup = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove_all(storageDir, ec);
        });

        auto settings = GetDefaultSessionSettings(c_sessionName);
        settings.StoragePath = storageDir.c_str();
        auto session = CreateSession(settings);

        // The VM starts lazily, so a freshly created session is already idle.
        BOOL wasAlreadyIdle = FALSE;
        VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_TRUE(wasAlreadyIdle);
        VERIFY_IS_FALSE(IsVmRunning(c_sessionName));

        // Starting a process brings the VM up.
        {
            WSLCProcessLauncher launcher("/bin/sleep", {"/bin/sleep", "60"});
            auto process = launcher.Launch(*session);
            VERIFY_IS_TRUE(IsVmRunning(c_sessionName));
        }

        // Releasing the process wrapper removes its activity hold, allowing idle termination.
        wasAlreadyIdle = TRUE;
        VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_FALSE(wasAlreadyIdle);
        VERIFY_IS_FALSE(IsVmRunning(c_sessionName));

        // A second trigger is now a no-op.
        wasAlreadyIdle = FALSE;
        VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_TRUE(wasAlreadyIdle);

        // The session survives and lazily restarts the VM on the next operation.
        WSLCProcessLauncher launcher2("/bin/sleep", {"/bin/sleep", "60"});
        auto process2 = launcher2.Launch(*session);
        VERIFY_IS_TRUE(IsVmRunning(c_sessionName));
    }

    // A tmpfs-backed session has no persistent storage, so its VM state cannot be recovered after a
    // teardown. TriggerIdleTermination must refuse to tear such a session down (matching the
    // automatic idle timer), leaving the VM running rather than destroying unrecoverable state.
    WSLC_TEST_METHOD(TriggerIdleTerminationRefusedWithoutStorage)
    {
        constexpr auto c_sessionName = L"wslc-idle-tmpfs-test";
        auto session = CreateSession(GetDefaultSessionSettings(c_sessionName));

        // A never-started session is already idle even though idle termination is disabled.
        BOOL wasAlreadyIdle = FALSE;
        VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_TRUE(wasAlreadyIdle);
        VERIFY_IS_FALSE(IsVmRunning(c_sessionName));

        WSLCProcessLauncher launcher("/bin/sleep", {"/bin/sleep", "60"});
        auto process = launcher.Launch(*session);
        VERIFY_IS_TRUE(IsVmRunning(c_sessionName));

        wasAlreadyIdle = TRUE;
        VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_FALSE(wasAlreadyIdle);

        // The VM must still be running: the tmpfs session was not torn down.
        VERIFY_IS_TRUE(IsVmRunning(c_sessionName));
    }

    // A running container pins the VM via its activity hold, matching the production idle timer.
    WSLC_TEST_METHOD(TriggerIdleTerminationDefersForRunningContainer)
    {
        WSLCContainerLauncher launcher("debian:latest", "wslc-idle-active", {"/bin/sleep", "600"});
        auto container = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        VERIFY_IS_TRUE(IsVmRunning(c_testSessionName));

        // The test hook honors the same activity guard as automatic idle teardown.
        BOOL wasAlreadyIdle = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_FALSE(wasAlreadyIdle);
        VERIFY_IS_TRUE(IsVmRunning(c_testSessionName));
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
    }

    // A created or stopped container has no activity hold. Its port mapping and bind mount must remain
    // usable after each idle teardown and lazy VM restart.
    WSLC_TEST_METHOD(TriggerIdleTerminationRecoversStoppedContainerResources)
    {
        const auto hostFolder = std::filesystem::current_path() / "test-idle-container-volume";
        std::filesystem::create_directories(hostFolder);
        VERIFY_IS_TRUE((std::ofstream(hostFolder / "marker.txt") << "idle-recovery").good());
        auto folderCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
        });

        WSLCContainerLauncher launcher(
            "python:3.12-alpine",
            "wslc-idle-resource-recovery",
            {"python3", "-m", "http.server", "8000", "--bind", "0.0.0.0", "--directory", "/data"},
            {"PYTHONUNBUFFERED=1"},
            "bridge");
        launcher.AddPort(1270, 8000, AF_INET);
        launcher.AddVolume(hostFolder.wstring(), "/data", true);
        auto container = launcher.Create(*m_defaultSession);

        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);

        // Tear down before the first start, then validate both recovered resources.
        BOOL wasAlreadyIdle = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_FALSE(wasAlreadyIdle);
        VERIFY_IS_FALSE(IsVmRunning(c_testSessionName));

        for (int iteration = 0; iteration < 2; ++iteration)
        {
            {
                VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));
                VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
                VERIFY_IS_TRUE(IsVmRunning(c_testSessionName));

                auto initProcess = container.GetInitProcess();
                WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

                const auto ports = container.Inspect().Ports;
                VERIFY_IS_TRUE(ports.contains("8000/tcp"));
                VERIFY_ARE_EQUAL(ports.at("8000/tcp").size(), 1u);
                VERIFY_ARE_EQUAL(ports.at("8000/tcp")[0].HostPort, std::string{"1270"});
                ExpectHttpResponse(L"http://127.0.0.1:1270/marker.txt", 200);

                VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
                VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
            }

            wasAlreadyIdle = TRUE;
            VERIFY_SUCCEEDED(m_defaultSession->TriggerIdleTermination(&wasAlreadyIdle));
            VERIFY_IS_FALSE(wasAlreadyIdle);
            VERIFY_IS_FALSE(IsVmRunning(c_testSessionName));
        }
    }

    // A container that outlives an idle teardown still owns VM-scoped state (bind mounts, port
    // relays) that is released from ~WSLCContainerImpl when the session is finally torn down. By
    // then the VM object is gone, and a graceful idle teardown leaves VmExited() false, so the
    // release path must key off "no VM" as well; a throw out of the destructor is unrecoverable
    // because destructors are noexcept and would terminate the session host.
    WSLC_TEST_METHOD(SessionTerminationAfterIdleTerminationWithContainer)
    {
        const auto hostFolder = std::filesystem::current_path() / "test-idle-terminate-volume";
        std::filesystem::create_directories(hostFolder);
        auto folderCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
        });

        WSLCContainerLauncher launcher("debian:latest", "wslc-idle-terminate-session", {"/bin/sleep", "600"});
        launcher.AddVolume(hostFolder.wstring(), "/data", true);

        {
            auto container = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_IS_TRUE(IsVmRunning(c_testSessionName));

            // Stop the container so it releases its activity hold on the VM.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Drop the client reference without deleting: the session keeps the only remaining
            // reference in m_containers, so the impl is destroyed by the session teardown below
            // rather than here (where the VM is still alive and the release path is trivially safe).
            container.SetDeleteOnClose(false);
        }

        // Tear the VM down. The container metadata (including its mounted-volume state) deliberately
        // survives, while the VM object is released without the exit event ever being signaled.
        BOOL wasAlreadyIdle = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->TriggerIdleTermination(&wasAlreadyIdle));
        VERIFY_IS_FALSE(wasAlreadyIdle);
        VERIFY_IS_FALSE(IsVmRunning(c_testSessionName));

        // Terminating clears m_containers, running ~WSLCContainerImpl with no VM to unmount from.
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        WaitForSessionTermination(m_defaultSession.get());

        {
            auto restore = ResetTestSession();
        }

        // The session host must still be alive and usable: if the destructor threw, the per-user
        // host process died and this fails.
        WSLCProcessLauncher processLauncher("/bin/echo", {"/bin/echo", "OK"});
        auto process = processLauncher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(process.Wait(), 0);
        PruneResult pruneResult;
        LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, &pruneResult.result));
    }

    // Hammer the idle-teardown path concurrently with VM-level operations to surface deadlocks or
    // stale-state races. Operations may fail while the VM is being torn down; that is tolerated, but
    // the workers must never hang and the session must remain usable afterwards.
    WSLC_TEST_METHOD(TriggerIdleTerminationConcurrentWithOperations)
    {
        constexpr auto c_sessionName = L"wslc-idle-hammer-test";

        // Idle termination only tears down storage-backed sessions, so a dedicated storage directory
        // is required for the teardown path to actually run under the concurrent hammering.
        const auto storageDir = std::filesystem::current_path() / "test-storage-idle-hammer";
        std::error_code storageError;
        std::filesystem::remove_all(storageDir, storageError);
        std::filesystem::create_directories(storageDir);
        auto storageCleanup = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove_all(storageDir, ec);
        });

        auto settings = GetDefaultSessionSettings(c_sessionName);
        settings.StoragePath = storageDir.c_str();
        auto session = CreateSession(settings);

        std::atomic<bool> stop = false;
        std::atomic<unsigned int> opFailures = 0;

        // Run the hammering worker via a future so the join is bounded: a real teardown/operation
        // deadlock would otherwise hang the test host indefinitely instead of failing. If the worker
        // does not drain within the timeout after we signal stop, treat it as a deadlock and fail.
        auto worker = std::async(std::launch::async, [&]() {
            while (!stop.load())
            {
                try
                {
                    WSLCProcessLauncher launcher("/bin/true", {"/bin/true"});
                    auto process = launcher.Launch(*session);
                    process.GetExitEvent().wait(5000);
                }
                catch (...)
                {
                    opFailures.fetch_add(1);
                }
            }
        });

        for (int i = 0; i < 25; ++i)
        {
            BOOL wasAlreadyIdle = FALSE;
            VERIFY_SUCCEEDED(session->TriggerIdleTermination(&wasAlreadyIdle));
        }

        stop.store(true);

        // A real teardown/operation deadlock would leave the worker wedged forever. The std::future
        // destructor blocks until the task completes, so letting a failed VERIFY unwind here would hang
        // the test host -- exactly what this test guards against. Fail fast with a dump on timeout so we
        // never unwind with an unfinished async task.
        FAIL_FAST_IF_MSG(
            worker.wait_for(std::chrono::seconds(60)) != std::future_status::ready,
            "hammering worker did not drain after stop; likely teardown/operation deadlock");

        worker.get();

        LogInfo("TriggerIdleTerminationConcurrentWithOperations tolerated %u operation failures", opFailures.load());

        // The session must still be usable after the hammering.
        WSLCProcessLauncher launcher("/bin/true", {"/bin/true"});
        auto process = launcher.Launch(*session);
        VERIFY_IS_TRUE(process.GetExitEvent().wait(30000));
    }

    // Helper: COM callback that captures all warnings received.
    class CapturingWarningCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWarningCallback, IFastRundown>
    {
    public:
        HRESULT OnWarning(LPCWSTR Message) override
        {
            std::lock_guard lock(m_lock);
            m_warnings.emplace_back(Message);
            return S_OK;
        }

        std::vector<std::wstring> GetWarnings()
        {
            std::lock_guard lock(m_lock);
            return m_warnings;
        }

    private:
        std::mutex m_lock;
        std::vector<std::wstring> m_warnings;
    };

    WSLC_TEST_METHOD(WarningCallbackContainerRecovery)
    {
        SKIP_TEST_SERVER();

        constexpr auto c_sessionName = L"warning-container-recovery";
        auto storagePath = (std::filesystem::current_path() / "test-warning-container-recovery").wstring();
        auto cleanupDir = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove_all(storagePath, ec);
        });

        // Phase 1: Create a session and inject a container with a corrupt WSLC metadata label via docker CLI.
        {
            auto settings = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings.StoragePath = storagePath.c_str();
            auto session = CreateSession(settings);

            // Load a base image so docker create works.
            LoadTestImage(*session, "hello-world:latest");

            // Create a container with an invalid WSLC metadata label.
            // RecoverExistingContainers will fail to parse this on the next session.
            auto result = ExpectCommandResult(
                session.get(),
                {"/usr/bin/docker", "create", "--label", "wslc.container.metadata=INVALID_JSON", "hello-world:latest"},
                0);

            // Capture the container ID from docker create output (stdout, trimmed).
            auto containerId = result.Output[1];
            containerId.erase(containerId.find_last_not_of(" \n\r") + 1);

            VERIFY_SUCCEEDED(session->Terminate());

            // Phase 2: Create a new session pointing to the same storage with a warning callback.
            auto warningCallback = Microsoft::WRL::Make<CapturingWarningCallback>();

            auto settings2 = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings2.StoragePath = storagePath.c_str();

            const auto sessionManager2 = OpenSessionManager();
            wil::com_ptr<IWSLCSession> session2;
            VERIFY_SUCCEEDED(sessionManager2->CreateSession(&settings2, WSLCSessionFlagsNone, warningCallback.Get(), &session2));
            wsl::windows::common::security::ConfigureForCOMImpersonation(session2.get());

            // The VM (and container recovery) starts lazily on the first operation. Trigger it via a
            // callback-bearing operation (CreateNetwork) so recovery warnings reach the warning callback.
            WSLCNetworkOptions triggerNetwork{};
            triggerNetwork.Name = "wslc-recovery-trigger";
            triggerNetwork.Driver = "bridge";
            VERIFY_SUCCEEDED(session2->CreateNetwork(&triggerNetwork, warningCallback.Get()));

            // Recovery runs during the lazy VM start under this operation's context, so the failure
            // warning is delivered to its warning callback.
            auto warnings = warningCallback->GetWarnings();
            auto recoveryWarning = std::format(
                L"wsl: {}\n",
                wsl::shared::Localization::MessageWslcFailedToRecoverContainer(wsl::shared::string::MultiByteToWide(containerId)));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == recoveryWarning; }));

            VERIFY_SUCCEEDED(session2->Terminate());
        }
    }

    WSLC_TEST_METHOD(WarningCallbackVolumeRecovery)
    {
        SKIP_TEST_SERVER();

        constexpr auto c_sessionName = L"warning-volume-recovery";
        auto storagePath = (std::filesystem::current_path() / "test-warning-volume-recovery").wstring();
        auto cleanupDir = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove_all(storagePath, ec);
        });

        std::string vhdHostPath;

        // Phase 1: Create a session with a VHD volume, then get the VHD path.
        {
            auto settings = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings.StoragePath = storagePath.c_str();
            auto session = CreateSession(settings);

            // Create a VHD volume.
            WSLCDriverOption driverOpts[] = {{"SizeBytes", "10485760"}}; // 10MB
            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = "wslc-test-warning-recovery";
            volumeOptions.Driver = "vhd";
            volumeOptions.DriverOpts = driverOpts;
            volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

            WSLCVolumeInformation volInfo{};
            VERIFY_SUCCEEDED(session->CreateVolume(&volumeOptions, &volInfo));

            // Inspect the volume to get the host VHD path.
            wil::unique_cotaskmem_ansistring inspectOutput;
            VERIFY_SUCCEEDED(session->InspectVolume("wslc-test-warning-recovery", &inspectOutput));
            auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(inspectOutput.get());
            VERIFY_IS_TRUE(inspect.Status.has_value());
            VERIFY_IS_TRUE(inspect.Status->contains("HostPath"));
            vhdHostPath = inspect.Status->at("HostPath");
            VERIFY_IS_FALSE(vhdHostPath.empty());

            VERIFY_SUCCEEDED(session->Terminate());
        }

        // Phase 2: Delete the VHD file, then restart with a warning callback.
        VERIFY_IS_TRUE(DeleteFileA(vhdHostPath.c_str()));

        {
            auto warningCallback = Microsoft::WRL::Make<CapturingWarningCallback>();

            auto settings = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings.StoragePath = storagePath.c_str();

            const auto sessionManager = OpenSessionManager();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_SUCCEEDED(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, warningCallback.Get(), &session));
            wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

            // The VM (and volume recovery) starts lazily on the first operation. Trigger it via a
            // callback-bearing operation (CreateNetwork) so recovery warnings reach the warning callback.
            WSLCNetworkOptions triggerNetwork{};
            triggerNetwork.Name = "wslc-recovery-trigger";
            triggerNetwork.Driver = "bridge";
            VERIFY_SUCCEEDED(session->CreateNetwork(&triggerNetwork, warningCallback.Get()));

            // Recovery runs during the lazy VM start under this operation's context, so the failure
            // warning is delivered to its warning callback.
            auto warnings = warningCallback->GetWarnings();
            auto recoveryWarning =
                std::format(L"wsl: {}\n", wsl::shared::Localization::MessageWslcFailedToRecoverVolume(L"wslc-test-warning-recovery"));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == recoveryWarning; }));

            // Clean up the orphaned volume from Docker's metadata.
            LOG_IF_FAILED(session->DeleteVolume("wslc-test-warning-recovery"));

            VERIFY_SUCCEEDED(session->Terminate());
        }
    }

    WSLC_TEST_METHOD(WarningCallbackGuestVolumeRecovery)
    {
        SKIP_TEST_SERVER();

        constexpr auto c_sessionName = L"warning-guest-volume-recovery";
        constexpr auto c_volumeName = "wslc-test-warning-guest-recovery";
        auto storagePath = (std::filesystem::current_path() / "test-warning-guest-volume-recovery").wstring();
        auto cleanupDir = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove_all(storagePath, ec);
        });

        // Create a session and, via the docker CLI, inject a "local" volume with driver options we don't support (type=nfs).
        // This bypasses our CreateVolume validation, leaving a volume that WSLCGuestVolumeImpl::Open will reject when the next
        // session recovers it.
        {
            auto settings = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings.StoragePath = storagePath.c_str();
            auto session = CreateSession(settings);

            ExpectCommandResult(
                session.get(),
                {"/usr/bin/docker",
                 "volume",
                 "create",
                 "--driver",
                 "local",
                 "--opt",
                 "type=nfs",
                 "--opt",
                 "o=addr=127.0.0.1,rw",
                 "--opt",
                 "device=:/exports/test",
                 c_volumeName},
                0);

            VERIFY_SUCCEEDED(session->Terminate());
        }

        // Restart with a warning callback and verify the unsupported volume triggers a recovery warning when the session loads.
        {
            auto warningCallback = Microsoft::WRL::Make<CapturingWarningCallback>();

            auto settings = GetDefaultSessionSettings(c_sessionName, false, WSLCNetworkingModeConsomme);
            settings.StoragePath = storagePath.c_str();

            const auto sessionManager = OpenSessionManager();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_SUCCEEDED(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, warningCallback.Get(), &session));
            wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

            // The VM (and guest volume recovery) starts lazily on the first operation. Trigger it via a
            // callback-bearing operation (CreateNetwork) so recovery warnings reach the warning callback.
            WSLCNetworkOptions triggerNetwork{};
            triggerNetwork.Name = "wslc-recovery-trigger";
            triggerNetwork.Driver = "bridge";
            VERIFY_SUCCEEDED(session->CreateNetwork(&triggerNetwork, warningCallback.Get()));

            // Recovery runs during the lazy VM start under this operation's context, so the failure
            // warning is delivered to its warning callback.
            auto warnings = warningCallback->GetWarnings();
            auto recoveryWarning = std::format(L"wsl: {}\n", wsl::shared::Localization::MessageWslcFailedToRecoverVolume(c_volumeName));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == recoveryWarning; }));

            // Clean up the volume from Docker's metadata.
            ExpectCommandResult(session.get(), {"/usr/bin/docker", "volume", "rm", "-f", c_volumeName}, 0);

            VERIFY_SUCCEEDED(session->Terminate());
        }
    }
};
