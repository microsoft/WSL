/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerLifecycleTests.cpp

Abstract:

    This file contains test cases for the WSLC container lifecycle API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerLifecycleTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerLifecycleTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    // Unhide the helpers shadowed by the test methods of the same name.
    using WSLCTestBase::OpenContainer;

    RunningWSLCContainer LaunchContainerWithBlockingStopHandler(const std::string& name)
    {
        WSLCContainerLauncher launcher(
            "debian:latest",
            name,
            {"/bin/sh", "-c", "trap 'echo stopping; read value; exit 0' TERM; echo ready; while true; do sleep 1; done"},
            {},
            "host",
            WSLCProcessFlagsStdin);

        return launcher.Launch(*m_defaultSession);
    }

    WSLC_TEST_METHOD(CreateContainer)
    {
        // Test a simple container start.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-simple", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});

            // Validate that GetInitProcess fails with the process argument is null.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), container.Get().GetInitProcess(nullptr));
        }

        // Validate that CreateContainer rejects a null image and invalid flags.
        {
            WSLCContainerOptions options{};
            wil::com_ptr<IWSLCContainer> container;

            // A null Image field is rejected with E_POINTER.
            VERIFY_ARE_EQUAL(E_POINTER, m_defaultSession->CreateContainer(&options, nullptr, &container));

            // Invalid container flags are rejected with E_INVALIDARG.
            options.Image = "debian:latest";
            options.Flags = static_cast<WSLCContainerFlags>(0x80);
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateContainer(&options, nullptr, &container));

            // Invalid init process flags are rejected with E_INVALIDARG.
            options.Flags = WSLCContainerFlagsNone;
            options.InitProcessOptions.Flags = static_cast<WSLCProcessFlags>(0x4);
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateContainer(&options, nullptr, &container));
        }

        // Validate that env is correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-env", {"/bin/sh", "-c", "echo $testenv"}, {{"testenv=testvalue"}});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that exit codes are correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-exit-code", {"/bin/sh", "-c", "exit 12"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {}, 12);
        }

        // Validate that stdin is correctly wired
        {
            WSLCContainerLauncher launcher("debian:latest", "test-default-entrypoint", {"/bin/cat"}, {}, "host", WSLCProcessFlagsStdin);

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
            WSLCContainerLauncher launcher("debian:latest", "test-stdin", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            process.GetStdHandle(0); // Close stdin;

            ValidateProcessOutput(process, {{1, ""}});
        }

        // Validate that the default stop signal is respected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-signal-1", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetDefaultStopSignal(WSLCSignalSIGHUP);
            launcher.SetContainerFlags(WSLCContainerFlagsInit);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalNone, 60));

            // Validate that the init process exited with the expected signal.
            VERIFY_ARE_EQUAL(process.Wait(), WSLCSignalSIGHUP + 128);
        }

        // Validate that the default stop signal can be overridden.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-signal-2", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetDefaultStopSignal(WSLCSignalSIGHUP);
            launcher.SetContainerFlags(WSLCContainerFlagsInit);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 60));

            // Validate that the init process exited with the expected signal.
            VERIFY_ARE_EQUAL(process.Wait(), WSLCSignalSIGKILL + 128);
        }

        // Validate that entrypoint is respected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-entrypoint", {"OK"});
            launcher.SetEntrypoint({"/bin/echo", "-n"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK"}});
        }

        // Validate that the working directory is correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-signal-1", {"pwd"});
            launcher.SetWorkingDirectory("/tmp");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "/tmp\n"}});
        }

        // Validate that the current directory is created if it doesn't exist.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-bad-cwd", {"pwd"});
            launcher.SetWorkingDirectory("/new-dir");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "/new-dir\n"}});
        }

        // Validate that hostname and domainanme are correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-hostname", {"/bin/sh", "-c", "echo $(hostname).$(domainname)"});

            launcher.SetHostname("my-host-name");
            launcher.SetDomainname("my-domain-name");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "my-host-name.my-domain-name\n"}});
        }

        // Validate that containers without DNS configuration use default DNS.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-no-dns", {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that custom DNS servers are correctly wired.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-dns-custom", {"/bin/grep", "-iF", "nameserver 1.2.3.4", "/etc/resolv.conf"});

            launcher.SetDnsServers({"1.2.3.4"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that custom DNS search domains are correctly wired.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-dns-search", {"/bin/grep", "-iF", "test.local", "/etc/resolv.conf"});

            launcher.SetDnsSearchDomains({"test.local"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that custom DNS options are correctly wired.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-dns-options", {"/bin/grep", "-iF", "timeout:1", "/etc/resolv.conf"});

            launcher.SetDnsOptions({"timeout:1"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that multiple DNS options are correctly wired.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-dns-options-multiple", {"/bin/grep", "-iF", "timeout:2", "/etc/resolv.conf"});

            launcher.SetDnsOptions({"timeout:1", "timeout:2"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that the username is correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-username", {"whoami"});

            launcher.SetUser("nobody");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "nobody\n"}});
        }

        // Validate that the group is correctly wired.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-group", {"groups"});

            launcher.SetUser("nobody:www-data");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "www-data\n"}});
        }

        // Validate that the container behaves correctly if the caller keeps a reference to an init process during termination.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-init-ref", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);

            auto container = launcher.Launch(*m_defaultSession);
            auto containerId = container.Id();

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                wil::com_ptr<IWSLCContainer> openedContainer;
                VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerId.c_str(), &openedContainer));
                VERIFY_SUCCEEDED(openedContainer->Delete(WSLCDeleteFlagsNone));
            });

            auto process = container.GetInitProcess();

            VERIFY_ARE_EQUAL(process.State(), WslcProcessStateRunning);

            // Terminate the session.
            ResetTestSession();

            WSLCProcessState processState{};
            int exitCode{};
            VERIFY_ARE_EQUAL(process.Get().GetState(&processState, &exitCode), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));

            WSLCContainerState state{};
            VERIFY_ARE_EQUAL(container.Get().GetState(&state), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));
        }

        // Validate error handling when the username / group doesn't exist
        {
            WSLCContainerLauncher launcher("debian:latest", "test-no-missing-user", {"groups"});

            launcher.SetUser("does-not-exist");

            auto [result, _] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(result, E_FAIL);

            ValidateCOMErrorMessage(L"unable to find user does-not-exist: no matching entries in passwd file");
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-empty-args", {"echo", "foo", "", "bar"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // Expect two spaces for the empty argument.
        }

        // Validate that tmpfs mounts are correctly wired.
        {
            WSLCContainerLauncher launcher(
                "debian:latest",
                "test-tmpfs",
                {"/bin/sh", "-c", "mount | grep 'tmpfs on /mnt/wslc-tmpfs1' && mount | grep 'tmpfs on /mnt/wslc-tmpfs2'"});

            launcher.AddTmpfs("/mnt/wslc-tmpfs1", "rw,noexec,nosuid,size=65536k");
            launcher.AddTmpfs("/mnt/wslc-tmpfs2", "");

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {}, 0);
        }

        // Validate that relative tmpfs paths are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-tmpfs-relative", {"/bin/cat"});
            launcher.AddTmpfs("relative-path", "");

            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);

            ValidateCOMErrorMessage(wsl::shared::Localization::WSLCCLI_MountTargetAbsoluteError());
        }

        // Validate that invalid tmpfs options are rejected by Docker.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-tmpfs-invalid-opts", {"/bin/cat"});
            launcher.AddTmpfs("/mnt/wslc-tmpfs", "invalid_option_xyz");

            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);

            ValidateCOMErrorMessage(L"invalid tmpfs option [\"invalid_option_xyz\"]");
        }

        // Validate error paths
        {
            WSLCContainerLauncher launcher("debian:latest", std::string(WSLC_MAX_CONTAINER_NAME_LENGTH + 1, 'a'), {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLCContainerLauncher launcher(std::string(WSLC_MAX_IMAGE_NAME_LENGTH + 1, 'a'), "dummy", {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);
        }

        {
            WSLCContainerLauncher launcher("invalid-image-name", "dummy", {"/bin/cat"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, WSLC_E_IMAGE_NOT_FOUND);
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "dummy", {"/does-not-exist"});
            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_INVALIDARG);

            ValidateCOMErrorMessage(
                L"failed to create task for container: failed to create shim task: OCI runtime create failed: runc create "
                L"failed: unable to start container process: error during container init: exec: \"/does-not-exist\": stat "
                L"/does-not-exist: no such file or directory: unknown");
        }

        // Test null image name
        {
            WSLCContainerOptions options{};
            options.Image = nullptr;
            options.Name = "test-container";
            options.InitProcessOptions.CommandLine = {.Values = nullptr, .Count = 0};

            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
            VERIFY_ARE_EQUAL(hr, E_POINTER);
        }

        // Test null container name
        {
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = nullptr;
            options.InitProcessOptions.CommandLine = {.Values = nullptr, .Count = 0};

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_SUCCEEDED(m_defaultSession->CreateContainer(&options, nullptr, &container));
            VERIFY_SUCCEEDED(container->Delete(WSLCDeleteFlagsNone));
        }

        // Validate that invalid tty sizes are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "invalid-tty-size-init", {"/bin/sh"}, {}, {}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);
            launcher.SetTtySize(0, 0);

            auto [result, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(result, E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerStartAfterStop)
    {
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});

            {
                // Validate that the container can be restarted.
                VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr), S_OK);
                auto restartedProcess = container.GetInitProcess();
                ValidateProcessOutput(restartedProcess, {{1, "OK\n"}});
            }

            {
                // Validate that the container can be restarted without the attach flag.
                VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), S_OK);
                auto restartedProcess = container.GetInitProcess();
                VERIFY_ARE_EQUAL(restartedProcess.Wait(), 0);

                COMOutputHandle stdoutLogs{};
                COMOutputHandle stderrLogs{};
                VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsNone, &stdoutLogs, &stderrLogs, 0, 0, 0));

                ValidateHandleOutput(stdoutLogs.Get(), "OK\nOK\nOK\n");
                ValidateHandleOutput(stderrLogs.Get(), "");
            }
        }

        // Validate that containers can be restarted after being explicitly stopped.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start-2", {"sleep", "99999"});
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto initProcess = container.GetInitProcess();
            initProcess.Get().Signal(WSLCSignalSIGKILL);
            VERIFY_ARE_EQUAL(initProcess.Wait(), WSLCSignalSIGKILL + 128);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Validate that deleted containers can't be started.
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), RPC_E_DISCONNECTED);
        }

        // Validate restart behavior for a container with the autorm flag set
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start-3", {"sleep", "99999"});
            launcher.SetContainerFlags(WSLCContainerFlagsRm);
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            // Validate that deleted containers can't be started.
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), RPC_E_DISCONNECTED);
        }

        // Validate that invalid start flags are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start-invalid-flags", {"echo", "OK"});
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.Get().Start(static_cast<WSLCContainerStartFlags>(0x2), nullptr, nullptr), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerRestart)
    {
        // A running container is stopped and started again, replacing its init process.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-running", {"sleep", "99999"});
            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto initProcess = container.GetInitProcess();
            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr));

            VERIFY_ARE_EQUAL(initProcess.Wait(), WSLCSignalSIGKILL + 128);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        }

        // A created container has no stop phase.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-created", {"sleep", "99999"});
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);

            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        }

        // An exited container is started again.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-exited", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);

            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OK\n"}});

            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr));

            auto restartedProcess = container.GetInitProcess();
            VERIFY_ARE_EQUAL(restartedProcess.Wait(), 0);

            COMOutputHandle stdoutLogs{};
            COMOutputHandle stderrLogs{};
            VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsNone, &stdoutLogs, &stderrLogs, 0, 0, 0));
            ValidateHandleOutput(stdoutLogs.Get(), "OK\nOK\n");
        }

        // Restarting a container with the autorm flag set must not auto-delete it, but a later stop must.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-autorm", {"sleep", "99999"});
            launcher.SetContainerFlags(WSLCContainerFlagsRm | WSLCContainerFlagsInit);
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGTERM, WSLC_STOP_TIMEOUT_DEFAULT, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), RPC_E_DISCONNECTED);
        }

        // Validate that deleted containers can't be restarted.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-deleted", {"sleep", "99999"});
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            VERIFY_ARE_EQUAL(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr), RPC_E_DISCONNECTED);
        }

        // Ports and mounts survive a restart: they are held across both phases rather than released and re-acquired.
        {
            const auto hostFolder = std::filesystem::current_path() / "test-restart-volume";
            std::filesystem::create_directories(hostFolder);
            VERIFY_IS_TRUE((std::ofstream(hostFolder / "marker.txt") << "restart-marker").good());
            auto folderCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                std::error_code ec;
                std::filesystem::remove_all(hostFolder, ec);
            });

            constexpr uint16_t hostPort = 1252;
            const std::string containerPort = "8000/tcp";
            const std::string volumePath = "/data";
            const auto markerUrl = std::format(L"http://127.0.0.1:{}/marker.txt", hostPort);

            WSLCContainerLauncher launcher(
                "python:3.12-alpine",
                "test-restart-ports-volumes",
                {"python3", "-m", "http.server", "8000", "--bind", "0.0.0.0", "--directory", volumePath},
                {"PYTHONUNBUFFERED=1"},
                "bridge");
            launcher.AddPort(hostPort, 8000, AF_INET);
            launcher.AddVolume(hostFolder.wstring(), volumePath, true);

            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");
            ExpectHttpResponse(markerUrl.c_str(), 200);

            // A start phase that re-reserved the host port would collide with the container's own reservation.
            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr));
            VERIFY_ARE_EQUAL(initProcess.Wait(), WSLCSignalSIGKILL + 128);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            const auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.Ports.contains(containerPort));
            VERIFY_ARE_EQUAL(inspect.Ports.at(containerPort).size(), 1u);
            VERIFY_ARE_EQUAL(std::to_string(hostPort), inspect.Ports.at(containerPort)[0].HostPort);

            VERIFY_ARE_EQUAL(inspect.Mounts.size(), 1u);
            VERIFY_ARE_EQUAL(inspect.Mounts[0].Destination, volumePath);
            VERIFY_IS_FALSE(inspect.Mounts[0].ReadWrite);
            VERIFY_ARE_EQUAL(inspect.Mounts[0].Type, "bind");

            // The restarted init has to bind again before the held relay has anything to forward to.
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() { ExpectHttpResponse(markerUrl.c_str(), 200); }, std::chrono::milliseconds(500), std::chrono::seconds(30));
        }

        // An init that ignores SIGTERM keeps the restart's stop phase in flight until the timeout expires,
        // which is what gives the requests below a window to land in the middle of a restart.
        const std::vector<std::string> ignoreStopSignal = {
            "/bin/sh", "-c", "trap 'echo stopping' TERM; while true; do sleep 1; done"};
        const std::string stopSignalMarker = "stopping";
        constexpr LONG stopTimeoutSeconds = 10;

        // A stop issued during a restart waits for both phases, so it can't be lost in between them.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-race-stop", ignoreStopSignal);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            std::promise<HRESULT> restartResult;
            std::thread restartThread(
                [&]() { restartResult.set_value(container.Get().Restart(WSLCSignalSIGTERM, stopTimeoutSeconds, nullptr)); });

            auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { restartThread.join(); });

            WaitForOutput(initProcess.GetStdHandle(1), stopSignalMarker);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(restartResult.get_future().get());
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
        }

        // A kill issued during a restart deliberately does not wait for it: it is what unblocks a stop phase
        // that an init like this one would otherwise keep in flight for the whole timeout.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-race-kill", ignoreStopSignal);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            std::promise<HRESULT> restartResult;
            std::thread restartThread(
                [&]() { restartResult.set_value(container.Get().Restart(WSLCSignalSIGTERM, stopTimeoutSeconds, nullptr)); });

            auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { restartThread.join(); });

            WaitForOutput(initProcess.GetStdHandle(1), stopSignalMarker);

            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
            VERIFY_SUCCEEDED(restartResult.get_future().get());
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        }

        // A delete issued during a restart deliberately does not wait for it, matching docker: whichever of
        // the delete and the restart's start phase lands first wins, and the other one fails.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-race-delete", ignoreStopSignal);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            std::promise<HRESULT> restartResult;
            std::thread restartThread(
                [&]() { restartResult.set_value(container.Get().Restart(WSLCSignalSIGTERM, stopTimeoutSeconds, nullptr)); });

            auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { restartThread.join(); });
            auto restartFuture = restartResult.get_future();

            WaitForOutput(initProcess.GetStdHandle(1), stopSignalMarker);

            // The gap between the two phases is short, so poll for it: until the container has exited, every
            // delete is turned away by the ordinary running-container guard rather than by the restart.
            const auto deleteResult = wsl::shared::retry::RetryWithTimeout<HRESULT>(
                [&]() {
                    const auto result = container.Get().Delete(WSLCDeleteFlagsNone);
                    THROW_HR_IF(
                        WSLC_E_CONTAINER_IS_RUNNING,
                        result == WSLC_E_CONTAINER_IS_RUNNING &&
                            restartFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
                    return result;
                },
                std::chrono::milliseconds(100),
                std::chrono::seconds(30));

            const auto restartHr = restartFuture.get();

            if (SUCCEEDED(deleteResult))
            {
                VERIFY_ARE_EQUAL(restartHr, WSLC_E_CONTAINER_DELETED);
            }
            else
            {
                // The start phase closed the gap first, so the container was running again by the last attempt.
                VERIFY_ARE_EQUAL(deleteResult, WSLC_E_CONTAINER_IS_RUNNING);
                VERIFY_SUCCEEDED(restartHr);
            }
        }

        // A force delete is not turned away by the running-container guard, so unlike the delete above it does
        // not have to wait for the gap between the phases: it lands while the stop phase is still in flight.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-race-force-delete", ignoreStopSignal);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            std::promise<HRESULT> restartResult;
            std::thread restartThread(
                [&]() { restartResult.set_value(container.Get().Restart(WSLCSignalSIGTERM, stopTimeoutSeconds, nullptr)); });

            auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { restartThread.join(); });

            WaitForOutput(initProcess.GetStdHandle(1), stopSignalMarker);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsForce));
            container.SetDeleteOnClose(false);
            VERIFY_ARE_EQUAL(restartResult.get_future().get(), WSLC_E_CONTAINER_DELETED);
        }

        // A restart issued during a restart waits for both of the first one's phases, so the two pairs
        // cannot interleave and the container is left running.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-restart-race-restart", ignoreStopSignal);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            std::promise<HRESULT> restartResult;
            std::thread restartThread(
                [&]() { restartResult.set_value(container.Get().Restart(WSLCSignalSIGTERM, stopTimeoutSeconds, nullptr)); });

            auto joinThread = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { restartThread.join(); });

            WaitForOutput(initProcess.GetStdHandle(1), stopSignalMarker);

            // The first restart is still in its stop phase, so this one only returns once that pair is done.
            VERIFY_SUCCEEDED(container.Get().Restart(WSLCSignalSIGKILL, 0, nullptr));
            VERIFY_SUCCEEDED(restartResult.get_future().get());
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        }
    }

    WSLC_TEST_METHOD(EventStream)
    {
        constexpr auto c_containerName = "wslc-test-events";
        constexpr auto c_imageName = "debian:latest";
        constexpr auto c_labelKey = "event-label";
        constexpr auto c_labelValue = "event-value";
        const auto expectedExitCode = std::to_string(128 + WSLCSignalSIGKILL);

        auto now = [] { return duration_cast<seconds>(system_clock::now().time_since_epoch()).count(); };

        // Drains a bounded event stream to completion (GetNext returns WSLC_E_EVENT_STREAM_FINISHED
        // once the until-time has passed and the backlog is exhausted), parsing each event's JSON.
        auto drain = [](IWSLCEventStream* stream) {
            std::vector<wsl::windows::common::wslc_schema::Event> events;

            wil::unique_cotaskmem_ansistring eventJson;
            HRESULT result;
            while (SUCCEEDED(result = stream->GetNext(nullptr, &eventJson)))
            {
                events.push_back(wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(eventJson.get()));
            }

            VERIFY_ARE_EQUAL(WSLC_E_EVENT_STREAM_FINISHED, result);
            return events;
        };

        // Verifies the given events match the expected actions in order for a given actor.
        auto verifyEvents = [&](const std::vector<wsl::windows::common::wslc_schema::Event>& events,
                                const std::string& actorId,
                                const std::vector<std::string>& expectedActions) {
            VERIFY_ARE_EQUAL(events.size(), expectedActions.size());

            for (size_t i = 0; i < expectedActions.size(); ++i)
            {
                const auto& action = expectedActions[i];
                const auto& event = events[i];

                VERIFY_ARE_EQUAL(action, event.Action);
                VERIFY_ARE_EQUAL(actorId, event.Actor.ID);
                VERIFY_ARE_EQUAL(c_containerName, event.Actor.Attributes.at("name"));
                VERIFY_ARE_EQUAL(c_imageName, event.Actor.Attributes.at("image"));
                VERIFY_ARE_EQUAL(c_labelValue, event.Actor.Attributes.at(c_labelKey));
                VERIFY_IS_FALSE(event.Actor.Attributes.contains("com.microsoft.wsl.container.metadata"));

                if (action == "stop")
                {
                    VERIFY_ARE_EQUAL(expectedExitCode, event.Actor.Attributes.at("exitCode"));
                }
                else
                {
                    VERIFY_IS_FALSE(event.Actor.Attributes.contains("exitCode"));
                }
            }
        };

        // Run a container through its create/start/kill/stop lifecycle inside a bounded time window.
        const LONGLONG since = now();
        std::string id;
        {
            WSLCContainerLauncher launcher(c_imageName, c_containerName, {"sleep", "99999"});
            launcher.AddLabel(c_labelKey, c_labelValue);
            auto container = launcher.Launch(*m_defaultSession);
            id = container.Id();

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Kill (rather than Stop) so Docker emits a 'kill' event ahead of the 'die' that stops it.
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
        }

        const LONGLONG until = now() + 1;
        std::vector<wsl::windows::common::wslc_schema::Event> lifecycleEvents;

        // The container's create, start, kill, stop, then destroy events are reported in order, each carrying
        // the container's 64-hex id as the actor.
        {
            WSLCFilter filter{"container", id.c_str()};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, &filter, 1, &stream));

            lifecycleEvents = drain(stream.get());
            verifyEvents(lifecycleEvents, id, {"create", "start", "kill", "stop", "destroy"});

            // The whole lifecycle falls inside the requested window.
            VERIFY_IS_TRUE(lifecycleEvents[0].time >= since);
            VERIFY_IS_TRUE(lifecycleEvents[4].time < until);
        }

        // Each lifecycle action is independently selectable: an 'event=<action>' filter, AND'd with
        // the container filter, returns exactly that one event out of the five recorded above.
        auto verifyEventFilter = [&](const char* action) {
            WSLCFilter filters[]{{"container", id.c_str()}, {"event", action}};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, filters, ARRAYSIZE(filters), &stream));

            verifyEvents(drain(stream.get()), id, {action});
        };

        verifyEventFilter("create");
        verifyEventFilter("start");
        verifyEventFilter("kill");
        verifyEventFilter("stop");
        verifyEventFilter("destroy");

        // Image filters match the image attribute carried by container events.
        {
            WSLCFilter filters[]{{"container", id.c_str()}, {"image", c_imageName}};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, filters, ARRAYSIZE(filters), &stream));

            verifyEvents(drain(stream.get()), id, {"create", "start", "kill", "stop", "destroy"});
        }

        // A non-matching image excludes the same container's events.
        {
            WSLCFilter filters[]{{"container", id.c_str()}, {"image", "nonexistent:image"}};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, filters, ARRAYSIZE(filters), &stream));

            VERIFY_IS_TRUE(drain(stream.get()).empty());
        }

        // Values sharing a filter key are OR'd.
        {
            WSLCFilter filters[]{{"container", id.c_str()}, {"event", "create"}, {"event", "destroy"}};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, filters, ARRAYSIZE(filters), &stream));

            verifyEvents(drain(stream.get()), id, {"create", "destroy"});
        }

        // Image events are not recorded yet, so a 'type=image' filter excludes the container's
        // events and leaves the stream empty.
        {
            WSLCFilter filter{"type", "image"};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, &filter, 1, &stream));

            VERIFY_IS_TRUE(drain(stream.get()).empty());
        }

        // An unmatched container id yields an empty stream, and GetNext validates its out-pointer.
        {
            WSLCFilter filter{"container", "0000000000000000000000000000000000000000000000000000000000000000"};
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, until, &filter, 1, &stream));

            VERIFY_IS_TRUE(drain(stream.get()).empty());
        }

        // A since-time later than a non-zero until-time describes a backwards window and is rejected.
        {
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->GetEvents(since + 1, since, nullptr, 0, &stream));
            ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcEventsInvalidTimeWindow(since + 1, since));
        }
    }

    WSLC_TEST_METHOD(EventStreamReportsLostEvents)
    {
        // One more than the store's ring capacity, so the reader's next slot is guaranteed evicted.
        constexpr size_t c_signalsToEvictReader = 257;

        WSLCContainerLauncher launcher("debian:latest", "wslc-test-event-stream-overrun", {"sleep", "99999"});
        auto container = launcher.Launch(*m_defaultSession);
        const auto id = container.Id();

        WSLCFilter filter{"container", id.c_str()};
        wil::com_ptr<IWSLCEventStream> stream;
        VERIFY_SUCCEEDED(m_defaultSession->GetEvents(0, 0, &filter, 1, &stream));

        // Read one event to place the reader's cursor inside the ring.
        wil::unique_cotaskmem_ansistring eventJson;
        VERIFY_SUCCEEDED(stream->GetNext(nullptr, &eventJson));
        const auto firstEvent = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(eventJson.get());
        VERIFY_ARE_EQUAL("create", firstEvent.Action);
        VERIFY_ARE_EQUAL(id, firstEvent.Actor.ID);

        // Docker emits a 'kill' event per signal. SIGWINCH is ignored by an unhandling init process, so the
        // container keeps running and each signal costs only one event.
        for (size_t i = 0; i < c_signalsToEvictReader; ++i)
        {
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGWINCH));
        }

        // Stopping waits for the 'die' event, which Docker delivers after every preceding 'kill'. Without this
        // barrier the reader could be checked before the ring has overrun it.
        VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

        VERIFY_ARE_EQUAL(WSLC_E_EVENTS_LOST, stream->GetNext(nullptr, &eventJson));

        // Reporting the gap resyncs the reader, so it resumes from the oldest event still buffered.
        VERIFY_SUCCEEDED(stream->GetNext(nullptr, &eventJson));
        const auto resumedEvent = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(eventJson.get());
        VERIFY_ARE_EQUAL("kill", resumedEvent.Action);
        VERIFY_ARE_EQUAL(id, resumedEvent.Actor.ID);
    }

    WSLC_TEST_METHOD(EventStreamSerializesConcurrentReaders)
    {
        constexpr auto c_containerName = "wslc-test-concurrent-event-readers";

        WSLCContainerLauncher launcher("debian:latest", c_containerName, {"sleep", "99999"});
        auto container = launcher.Launch(*m_defaultSession);
        const auto id = container.Id();

        WSLCFilter filters[]{{"container", id.c_str()}, {"event", "kill"}};
        wil::com_ptr<IWSLCEventStream> stream;

        // The window doubles as a hang guard, so it must comfortably outlast the waits below.
        const LONGLONG until = duration_cast<seconds>(system_clock::now().time_since_epoch()).count() + 120;
        VERIFY_SUCCEEDED(m_defaultSession->GetEvents(0, until, filters, ARRAYSIZE(filters), &stream));

        wil::unique_cotaskmem_ansistring firstEventJson;
        wil::unique_cotaskmem_ansistring secondEventJson;
        HRESULT firstResult{};
        HRESULT secondResult{};
        wil::unique_event firstReaderStarted{wil::EventOptions::ManualReset};
        wil::unique_event secondReaderStarted{wil::EventOptions::ManualReset};
        std::thread firstReader;
        std::thread secondReader;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(container.Get().Kill(WSLCSignalSIGWINCH));
            LOG_IF_FAILED(container.Get().Kill(WSLCSignalSIGWINCH));

            if (firstReader.joinable())
            {
                firstReader.join();
            }

            if (secondReader.joinable())
            {
                secondReader.join();
            }
        });

        firstReader = std::thread([&]() {
            firstReaderStarted.SetEvent();
            firstResult = stream->GetNext(nullptr, &firstEventJson);
        });
        VERIFY_IS_TRUE(firstReaderStarted.wait(30 * 1000));
        VERIFY_ARE_EQUAL(WAIT_TIMEOUT, WaitForSingleObject(firstReader.native_handle(), 100));

        secondReader = std::thread([&]() {
            secondReaderStarted.SetEvent();
            secondResult = stream->GetNext(nullptr, &secondEventJson);
        });
        VERIFY_IS_TRUE(secondReaderStarted.wait(30 * 1000));
        VERIFY_ARE_EQUAL(WAIT_TIMEOUT, WaitForSingleObject(secondReader.native_handle(), 100));

        HANDLE readers[]{firstReader.native_handle(), secondReader.native_handle()};
        VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGWINCH));

        const DWORD completedReader = WaitForMultipleObjects(ARRAYSIZE(readers), readers, FALSE, 30 * 1000);
        VERIFY_IS_TRUE(completedReader == WAIT_OBJECT_0 || completedReader == WAIT_OBJECT_0 + 1);

        // One event completes exactly one call; the other stays serialized until another event arrives.
        const DWORD pendingReader = completedReader == WAIT_OBJECT_0 ? 1 : 0;
        VERIFY_ARE_EQUAL(WAIT_TIMEOUT, WaitForSingleObject(readers[pendingReader], 100));

        VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGWINCH));
        VERIFY_ARE_EQUAL(WAIT_OBJECT_0, WaitForMultipleObjects(ARRAYSIZE(readers), readers, TRUE, 30 * 1000));

        firstReader.join();
        secondReader.join();
        cleanup.release();

        VERIFY_SUCCEEDED(firstResult);
        VERIFY_SUCCEEDED(secondResult);

        const auto firstEvent = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(firstEventJson.get());
        const auto secondEvent = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(secondEventJson.get());
        VERIFY_ARE_EQUAL("kill", firstEvent.Action);
        VERIFY_ARE_EQUAL(id, firstEvent.Actor.ID);
        VERIFY_ARE_EQUAL("kill", secondEvent.Action);
        VERIFY_ARE_EQUAL(id, secondEvent.Actor.ID);
    }

    WSLC_TEST_METHOD(EventStreamSessionTerminationAbortsReader)
    {
        WSLCFilter filter{"type", "container"};
        const LONGLONG since = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        wil::com_ptr<IWSLCEventStream> stream;
        VERIFY_SUCCEEDED(m_defaultSession->GetEvents(since, 0, &filter, 1, &stream));

        std::promise<HRESULT> getNextResult;
        std::thread readerThread([&]() {
            wil::unique_cotaskmem_ansistring eventJson;
            getNextResult.set_value(stream->GetNext(nullptr, &eventJson));
        });
        auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { readerThread.join(); });

        auto future = getNextResult.get_future();

        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        auto restore = ResetTestSession();

        // Termination wakes the parked reader; it must finish quickly and report E_ABORT.
        FAIL_FAST_IF_MSG(
            future.wait_for(10s) != std::future_status::ready, "event stream reader did not abort after session termination");
        VERIFY_ARE_EQUAL(E_ABORT, future.get());
    }

    WSLC_TEST_METHOD(EventStreamCancellationAbortsReader)
    {
        WSLCFilter filter{"container", "nonexistent-event-stream-container"};
        const LONGLONG now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
        const std::array<LONGLONG, 2> untilTimes{0, now + 120};

        for (const auto until : untilTimes)
        {
            wil::com_ptr<IWSLCEventStream> stream;
            VERIFY_SUCCEEDED(m_defaultSession->GetEvents(0, until, &filter, 1, &stream));

            std::promise<HRESULT> getNextResult;
            wil::unique_event readerStarted{wil::EventOptions::ManualReset};
            wil::unique_event cancelEvent{wil::EventOptions::ManualReset};
            wil::unique_cotaskmem_ansistring eventJson;
            std::thread readerThread([&]() {
                const auto coInitialize = wil::CoInitializeEx();
                readerStarted.SetEvent();
                getNextResult.set_value(stream->GetNext(cancelEvent.get(), &eventJson));
            });
            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                cancelEvent.SetEvent();
                FAIL_FAST_IF_MSG(
                    WaitForSingleObject(readerThread.native_handle(), 10 * 1000) != WAIT_OBJECT_0,
                    "event stream reader did not finish after cancellation");
                readerThread.join();
            });

            VERIFY_IS_TRUE(readerStarted.wait(30 * 1000));
            VERIFY_ARE_EQUAL(WAIT_TIMEOUT, WaitForSingleObject(readerThread.native_handle(), 100));
            cancelEvent.SetEvent();

            auto future = getNextResult.get_future();
            FAIL_FAST_IF_MSG(
                future.wait_for(10s) != std::future_status::ready, "event stream reader did not finish after cancellation");
            VERIFY_ARE_EQUAL(E_ABORT, future.get());
            VERIFY_IS_NULL(eventJson.get());
        }
    }

    WSLC_TEST_METHOD(EventStreamCancellationPreservesBufferedEvents)
    {
        WSLCContainerLauncher launcher("debian:latest", "wslc-test-event-cancellation", {"sleep", "99999"});
        auto container = launcher.Launch(*m_defaultSession);
        const auto id = container.Id();

        WSLCFilter filter{"container", id.c_str()};
        const LONGLONG until = duration_cast<seconds>(system_clock::now().time_since_epoch()).count() + 1;
        wil::com_ptr<IWSLCEventStream> stream;
        wil::com_ptr<IWSLCEventStream> otherStream;
        VERIFY_SUCCEEDED(m_defaultSession->GetEvents(0, until, &filter, 1, &stream));
        VERIFY_SUCCEEDED(m_defaultSession->GetEvents(0, until, &filter, 1, &otherStream));

        // Expiring the window must not discard buffered events from before the deadline.
        std::this_thread::sleep_until(sys_seconds{seconds{until}});

        wil::unique_event cancelEvent{wil::EventOptions::ManualReset};
        for (const auto* action : {"create", "start"})
        {
            cancelEvent.SetEvent();
            wil::unique_cotaskmem_ansistring eventJson;
            VERIFY_ARE_EQUAL(E_ABORT, stream->GetNext(cancelEvent.get(), &eventJson));
            VERIFY_IS_NULL(eventJson.get());

            // Cancelling one subscription must neither affect another nor consume its own next event.
            wil::unique_cotaskmem_ansistring otherEventJson;
            VERIFY_SUCCEEDED(otherStream->GetNext(nullptr, &otherEventJson));

            cancelEvent.ResetEvent();
            VERIFY_SUCCEEDED(stream->GetNext(cancelEvent.get(), &eventJson));
            VERIFY_ARE_EQUAL(std::string{otherEventJson.get()}, std::string{eventJson.get()});
            const auto event = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Event>(eventJson.get());
            VERIFY_ARE_EQUAL(action, event.Action);
            VERIFY_ARE_EQUAL(id, event.Actor.ID);
        }

        wil::unique_cotaskmem_ansistring eventJson;
        VERIFY_ARE_EQUAL(WSLC_E_EVENT_STREAM_FINISHED, stream->GetNext(cancelEvent.get(), &eventJson));
        VERIFY_IS_NULL(eventJson.get());

        cancelEvent.SetEvent();
        VERIFY_ARE_EQUAL(E_ABORT, stream->GetNext(cancelEvent.get(), &eventJson));
        VERIFY_IS_NULL(eventJson.get());
    }

    WSLC_TEST_METHOD(OpenContainer)
    {
        auto expectOpen = [&](const char* Id, HRESULT expectedResult = S_OK) {
            wil::com_ptr<IWSLCContainer> container;
            auto result = m_defaultSession->OpenContainer(Id, &container);

            VERIFY_ARE_EQUAL(result, expectedResult);

            return container;
        };

        {
            WSLCContainerLauncher launcher("debian:latest", "named-container", {"echo", "OK"});
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            VERIFY_ARE_EQUAL(container->Id().length(), WSLC_CONTAINER_ID_LENGTH);

            VERIFY_ARE_EQUAL(container->Name(), "named-container");

            // Validate that the container can be opened by name.
            expectOpen("named-container");

            // Validate that the container can be opened by ID.
            expectOpen(container->Id().c_str());

            // Validate that the container can be opened by a prefix of the ID.
            expectOpen(container->Id().substr(0, 8).c_str());
            expectOpen(container->Id().substr(0, 1).c_str());

            // Validate that prefix conflicts are correctly handled.
            std::vector<RunningWSLCContainer> createdContainers;
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

                auto [result, newContainer] = WSLCContainerLauncher("debian:latest").CreateNoThrow(*m_defaultSession);
                VERIFY_SUCCEEDED(result);

                createdContainers.emplace_back(std::move(newContainer.value()));
                char conflictChar = findConflict();
                if (conflictChar == '\0')
                {
                    continue;
                }

                expectOpen(std::string{&conflictChar, 1}.c_str(), WSLC_E_CONTAINER_PREFIX_AMBIGUOUS);
                break;
            }
        }

        // Test error paths
        {
            // A null id and a null output pointer are rejected by the marshaller.
            {
                wil::com_ptr<IWSLCContainer> container;
                VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->OpenContainer(nullptr, &container));
                VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->OpenContainer("named-container", nullptr));
            }

            expectOpen("", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: ''");

            expectOpen("non-existing-container", WSLC_E_CONTAINER_NOT_FOUND);
            ValidateCOMErrorMessage(L"Container 'non-existing-container' not found.");

            expectOpen("/", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: '/'");

            expectOpen("?foo=bar", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: '?foo=bar'");

            expectOpen("\n", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: '\n'");

            expectOpen(" ", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: ' '");
        }
    }

    WSLC_TEST_METHOD(ContainerState)
    {
        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLCContainerState>>& expectedContainers) {
            auto [containers, ports] = ListContainers(m_defaultSession.get());
            VERIFY_ARE_EQUAL(expectedContainers.size(), containers.size());

            for (size_t i = 0; i < expectedContainers.size(); i++)
            {
                const auto& [expectedName, expectedImage, expectedState] = expectedContainers[i];
                VERIFY_ARE_EQUAL(expectedName, containers[i].Name);
                VERIFY_ARE_EQUAL(expectedImage, containers[i].Image);
                VERIFY_ARE_EQUAL(expectedState, containers[i].State);
                VERIFY_ARE_EQUAL(strlen(containers[i].Id), WSLC_CONTAINER_ID_LENGTH);
                VERIFY_IS_TRUE(containers[i].StateChangedAt > 0);
                VERIFY_IS_TRUE(containers[i].CreatedAt > 0);
            }
        };

        {
            // Validate that the container list is initially empty.
            expectContainerList({});

            // Start one container and wait for it to exit.
            {
                WSLCContainerLauncher launcher("debian:latest", "exited-container", {"echo", "OK"});
                auto container = launcher.Launch(*m_defaultSession);
                auto process = container.GetInitProcess();

                ValidateProcessOutput(process, {{1, "OK\n"}});
                expectContainerList({{"exited-container", "debian:latest", WslcContainerStateExited}});
            }

            // Create a stuck container.
            WSLCContainerLauncher launcher("debian:latest", "test-container-1", {"sleep", "99999"});

            auto container = launcher.Launch(*m_defaultSession);

            // Verify that the container is in running state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            expectContainerList({{"test-container-1", "debian:latest", WslcContainerStateRunning}});

            // Capture StateChangedAt and CreatedAt while the container is running.
            LONGLONG runningStateChangedAt{};
            LONGLONG runningCreatedAt{};
            {
                auto [containers, ports] = ListContainers(m_defaultSession.get());
                VERIFY_ARE_EQUAL(containers.size(), 1);
                runningStateChangedAt = containers[0].StateChangedAt;
                runningCreatedAt = containers[0].CreatedAt;
                VERIFY_IS_TRUE(runningStateChangedAt > 0);
                VERIFY_IS_TRUE(runningCreatedAt > 0);
            }

            // Kill the container init process and expect it to be in exited state.
            auto initProcess = container.GetInitProcess();
            VERIFY_SUCCEEDED(initProcess.Get().Signal(WSLCSignalSIGKILL));

            // Wait for the process to actually exit.
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    initProcess.GetExitCode(); // Throw if the process hasn't exited yet.
                },
                std::chrono::milliseconds{100},
                std::chrono::seconds{30});

            // Expect the container to be in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
            expectContainerList({{"test-container-1", "debian:latest", WslcContainerStateExited}});

            // Verify that StateChangedAt was updated after the state transition.
            {
                auto [containers, ports] = ListContainers(m_defaultSession.get());
                VERIFY_ARE_EQUAL(containers.size(), 1);

                auto now = static_cast<LONGLONG>(time(nullptr));
                VERIFY_IS_TRUE(containers[0].StateChangedAt <= now);
                VERIFY_IS_TRUE(containers[0].StateChangedAt >= runningStateChangedAt);

                // CreatedAt must not change after state transitions.
                VERIFY_ARE_EQUAL(containers[0].CreatedAt, runningCreatedAt);
            }

            // Open a new reference to the same container.
            wil::com_ptr<IWSLCContainer> sameContainer;
            VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("test-container-1", &sameContainer));

            // Verify that the state matches.
            WSLCContainerState state{};
            VERIFY_SUCCEEDED(sameContainer->GetState(&state));
            VERIFY_ARE_EQUAL(state, WslcContainerStateExited);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Test StopContainer
        {
            // Create a container
            WSLCContainerLauncher launcher("debian:latest", "test-container-2", {"sleep", "99999"});

            auto container = launcher.Create(*m_defaultSession);

            // Validate that a created container cannot be stopped.

            auto id = container.Id();
            VERIFY_ARE_EQUAL(container.Get().Stop(WSLCSignalSIGKILL, 0), WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));

            // Verify that the container is in running state.
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-container-2", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            expectContainerList({});
        }

        // test StopContainer with custom timeouts.
        // N.B. We can't validate the actual timeouts since the tests environment will affect container stop times.
        {
            {
                // Create a container with a no stop timeout.
                WSLCContainerLauncher launcher("debian:latest", "test-container-stop-timeout-1", {"sleep", "99999"});
                launcher.SetStopTimeout(WSLC_STOP_TIMEOUT_NONE);

                auto container = launcher.Launch(*m_defaultSession);

                auto inspect = container.Inspect();
                VERIFY_ARE_EQUAL(inspect.Config.StopTimeout.value_or(0), WSLC_STOP_TIMEOUT_NONE);

                // Validate that passing '0' as the stop timeout overrides the default
                VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalNone, 0));
            }

            {
                // Create a container with an instant stop timeout.
                WSLCContainerLauncher launcher("debian:latest", "test-container-stop-timeout-2", {"sleep", "99999"});
                launcher.SetStopTimeout(0);

                auto container = launcher.Create(*m_defaultSession);

                auto inspect = container.Inspect();
                VERIFY_ARE_EQUAL(inspect.Config.StopTimeout.value_or(-1), 0);
            }

            {
                // Create a container with an short stop timeout.
                WSLCContainerLauncher launcher("debian:latest", "test-container-stop-timeout-3", {"sleep", "99999"});
                launcher.SetStopTimeout(1);

                auto container = launcher.Launch(*m_defaultSession);

                auto inspect = container.Inspect();
                VERIFY_ARE_EQUAL(inspect.Config.StopTimeout.value_or(0), 1);

                auto initProcess = container.GetInitProcess();
                std::thread stopThread([&]() { VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalNone, -1)); });

                auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                    LOG_IF_FAILED(container.Get().Kill(WSLCSignalSIGKILL));

                    if (stopThread.joinable())
                    {
                        stopThread.join();
                    }
                });

                // Wait for at least 2 seconds for the stop to complete to prove that the default 1 second timeout was correctly overridden.
                auto waitResult = WaitForSingleObject(stopThread.native_handle(), 2000);

                VERIFY_ARE_EQUAL(waitResult, WAIT_TIMEOUT);
            }
        }

        // Validate that health check options are forwarded to the container configuration.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-health", {"sleep", "99999"});
            launcher.SetHealthCmd("exit 0");
            launcher.SetHealthInterval(5'000'000'000LL);    // 5s
            launcher.SetHealthTimeout(3'000'000'000LL);     // 3s
            launcher.SetHealthStartPeriod(1'000'000'000LL); // 1s
            launcher.SetHealthRetries(2);

            auto container = launcher.Create(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"CMD-SHELL", "exit 0"};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());
            VERIFY_ARE_EQUAL(5'000'000'000LL, health.Interval.value_or(0));
            VERIFY_ARE_EQUAL(3'000'000'000LL, health.Timeout.value_or(0));
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.StartPeriod.value_or(0));
            VERIFY_ARE_EQUAL(2, health.Retries.value_or(0));
        }

        // Validate that a container without health options reports no health check.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-no-health", {"sleep", "99999"});

            auto container = launcher.Create(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_FALSE(inspect.Config.Healthcheck.has_value());
        }

        // Validate that Kill() works as expected
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-kill", {"sleep", "99999"}, {});

            auto container = launcher.Create(*m_defaultSession);

            // Validate that a created container cannot be killed.
            auto id = container.Id();
            VERIFY_ARE_EQUAL(container.Get().Kill(WSLCSignalNone), WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalNone));

            // Verify that the container is in exited state.
            expectContainerList({{"test-container-kill", "debian:latest", WslcContainerStateExited}});

            // Validate that killing a non-running container fails (unlike Stop())
            VERIFY_ARE_EQUAL(container.Get().Kill(WSLCSignalNone), WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));

            // Verify that deleting a container stopped via Kill() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            expectContainerList({});
        }

        // Validate that Kill() works with non-sigkill signals.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-kill-2", {"sleep", "99999"}, {});
            launcher.SetContainerFlags(WSLCContainerFlagsInit);

            auto container = launcher.Create(*m_defaultSession);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGTERM));

            VERIFY_ARE_EQUAL(container.GetInitProcess().Wait(120 * 1000), WSLCSignalSIGTERM + 128);

            // Verify that the container is in exited state.
            expectContainerList({{"test-container-kill-2", "debian:latest", WslcContainerStateExited}});
        }

        // Verify that trying to open a non existing container fails.
        {
            wil::com_ptr<IWSLCContainer> sameContainer;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("does-not-exist", &sameContainer), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Validate that container names are unique.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-unique-name", {"sleep", "99999"}, {}, "host");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Validate that a container with the same name cannot be started
            VERIFY_ARE_EQUAL(
                WSLCContainerLauncher("debian:latest", "test-unique-name", {"echo", "OK"}).LaunchNoThrow(*m_defaultSession).first,
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that running containers can't be deleted.
            auto id = container.Id();
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), WSLC_E_CONTAINER_IS_RUNNING);
            ValidateCOMErrorMessage(
                std::format(L"Container '{}' is running and cannot be removed. Either stop the container before removing or use forced remove (-f).", id));

            // Kill the container.
            auto initProcess = container.GetInitProcess();
            initProcess.Get().Signal(WSLCSignalSIGKILL);

            // Wait for the process to actually exit.
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    initProcess.GetExitCode(); // Throw if the process hasn't exited yet.
                },
                std::chrono::milliseconds{100},
                std::chrono::seconds{30});

            expectContainerList({{"test-unique-name", "debian:latest", WslcContainerStateExited}});

            // Verify that calling Stop() on exited containers is a no-op and state remains as WslcContainerStateExited.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that stopped containers can be deleted.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Verify that stopping a deleted container returns ERROR_INVALID_STATE.
            VERIFY_ARE_EQUAL(container.Get().Stop(WSLCSignalSIGTERM, 0), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers can't be deleted again.
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Verify that deleted containers don't show up in the container list.
            expectContainerList({});

            // Verify that the same name can be reused now that the container is deleted.
            WSLCContainerLauncher otherLauncher("debian:latest", "test-unique-name", {"echo", "OK"}, {}, "host");

            auto result = otherLauncher.Launch(*m_defaultSession).GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Code, 0);
        }

        // Validate that creating and starting a container separately behaves as expected

        {
            WSLCContainerLauncher launcher("debian:latest", "test-create", {"sleep", "99999"}, {});
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateCreated);
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));

            // Verify that Start() can't be called again on a running container.
            auto id = container->Id();
            VERIFY_ARE_EQUAL(container->Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), WSLC_E_CONTAINER_IS_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is running.", id));

            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container->Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateExited);

            VERIFY_SUCCEEDED(container->Get().Delete(WSLCDeleteFlagsNone));
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateDeleted);

            VERIFY_ARE_EQUAL(container->Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);
        }

        // Validate that containers behave correctly if they outlive their session.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-dangling-ref", {"sleep", "99999"}, {});
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Delete the container to avoid leaving it dangling after test completion.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Terminate the session
            ResetTestSession();

            // Validate that calling into the container returns RPC_S_SERVER_UNAVAILABLE.
            WSLCContainerState state = WslcContainerStateRunning;
            VERIFY_ARE_EQUAL(container.Get().GetState(&state), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));
            VERIFY_ARE_EQUAL(state, WslcContainerStateInvalid);
        }
    }

    WSLC_TEST_METHOD(DeleteContainer)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-container-delete", {"sleep", "99999"});

        {
            // Verify that a created container can be deleted.
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Verify that a deleted container can't be deleted again.
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));
        }

        {
            // Verify that a running container can't be deleted by default.
            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto id = container.Id();
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), WSLC_E_CONTAINER_IS_RUNNING);
            ValidateCOMErrorMessage(
                std::format(L"Container '{}' is running and cannot be removed. Either stop the container before removing or use forced remove (-f).", id));

            // Validate that invalid flags are rejected.
            VERIFY_ARE_EQUAL(container.Get().Delete(static_cast<WSLCDeleteFlags>(0x4)), E_INVALIDARG);

            // Verify that a running container can be deleted with the force flag.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsForce));
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsForce), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));
        }
    }

    WSLC_TEST_METHOD(ConcurrentContainerStopAndKill)
    {
        auto container = LaunchContainerWithBlockingStopHandler("test-concurrent-container-stops");
        auto initProcess = container.GetInitProcess();
        auto input = initProcess.GetStdHandle(0);
        auto outputHandle = initProcess.GetStdHandle(1);
        PartialHandleRead output{outputHandle.Get()};
        output.ExpectConsume("ready\n");

        HRESULT stopResult{};
        HRESULT killResult{};
        std::thread stopThread;
        std::thread killThread;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            input.Reset();

            if (stopThread.joinable())
            {
                stopThread.join();
            }

            if (killThread.joinable())
            {
                killThread.join();
            }
        });

        stopThread = std::thread([&]() { stopResult = container.Get().Stop(WSLCSignalSIGTERM, WSLC_STOP_TIMEOUT_NONE); });

        output.ExpectConsume("stopping\n");

        // A second lifecycle request must reach Docker while the indefinite Stop request is blocked.
        wil::unique_event killStarted{wil::EventOptions::ManualReset};
        killThread = std::thread([&]() {
            killStarted.SetEvent();
            killResult = container.Get().Kill(WSLCSignalSIGTERM);
        });

        VERIFY_IS_TRUE(killStarted.wait(30 * 1000));
        VERIFY_ARE_EQUAL(WaitForSingleObject(killThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        killThread.join();
        VERIFY_SUCCEEDED(killResult);
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 100), WAIT_TIMEOUT);

        const char stopInput = '\n';
        DWORD bytesWritten{};
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(input.Get(), &stopInput, sizeof(stopInput), &bytesWritten, nullptr));
        VERIFY_ARE_EQUAL(bytesWritten, static_cast<DWORD>(sizeof(stopInput)));
        input.Reset();

        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);

        stopThread.join();
        cleanup.release();

        VERIFY_SUCCEEDED(stopResult);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }

    WSLC_TEST_METHOD(ConcurrentContainerStopAndIgnoredSignal)
    {
        // The init process blocks in its SIGTERM handler and only logs SIGUSR1, so the Stop stays pending
        // across the Kill instead of being completed by it.
        WSLCContainerLauncher launcher(
            "debian:latest",
            "test-concurrent-stop-ignored-signal",
            {"/bin/sh",
             "-c",
             "trap 'echo stopping; while true; do sleep 1; done' TERM; trap 'echo signaled' USR1; echo ready; while true; do "
             "sleep 1; done"},
            {},
            "host");

        auto container = launcher.Launch(*m_defaultSession);
        auto initProcess = container.GetInitProcess();
        auto outputHandle = initProcess.GetStdHandle(1);
        PartialHandleRead output{outputHandle.Get()};
        output.ExpectConsume("ready\n");

        HRESULT stopResult{};
        HRESULT killResult{};
        std::thread stopThread;
        std::thread killThread;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(container.Get().Kill(WSLCSignalSIGKILL));

            if (stopThread.joinable())
            {
                stopThread.join();
            }

            if (killThread.joinable())
            {
                killThread.join();
            }
        });

        stopThread = std::thread([&]() { stopResult = container.Get().Stop(WSLCSignalSIGTERM, WSLC_STOP_TIMEOUT_NONE); });

        output.ExpectConsume("stopping\n");

        // A signal that doesn't stop the container must not wait on the Stop it raced.
        killThread = std::thread([&]() { killResult = container.Get().Kill(WSLCSignalSIGUSR1); });

        VERIFY_ARE_EQUAL(WaitForSingleObject(killThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        killThread.join();
        VERIFY_SUCCEEDED(killResult);

        // The signal reached the container, and the Stop is still pending.
        output.ExpectConsume("signaled\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 100), WAIT_TIMEOUT);

        VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));

        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        stopThread.join();
        cleanup.release();

        VERIFY_SUCCEEDED(stopResult);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }

    WSLC_TEST_METHOD(ConcurrentContainerStopTimeoutOverride)
    {
        auto container = LaunchContainerWithBlockingStopHandler("test-concurrent-stop-timeout");
        auto initProcess = container.GetInitProcess();
        auto input = initProcess.GetStdHandle(0);
        auto outputHandle = initProcess.GetStdHandle(1);
        PartialHandleRead output{outputHandle.Get()};
        output.ExpectConsume("ready\n");

        HRESULT indefiniteStopResult{};
        HRESULT immediateStopResult{};
        std::thread indefiniteStopThread;
        std::thread immediateStopThread;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            input.Reset();

            if (indefiniteStopThread.joinable())
            {
                indefiniteStopThread.join();
            }

            if (immediateStopThread.joinable())
            {
                immediateStopThread.join();
            }
        });

        indefiniteStopThread =
            std::thread([&]() { indefiniteStopResult = container.Get().Stop(WSLCSignalNone, WSLC_STOP_TIMEOUT_NONE); });

        output.ExpectConsume("stopping\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(indefiniteStopThread.native_handle(), 100), WAIT_TIMEOUT);

        immediateStopThread = std::thread([&]() { immediateStopResult = container.Get().Stop(WSLCSignalNone, 0); });

        VERIFY_ARE_EQUAL(WaitForSingleObject(immediateStopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        VERIFY_ARE_EQUAL(WaitForSingleObject(indefiniteStopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);

        indefiniteStopThread.join();
        immediateStopThread.join();
        cleanup.release();

        VERIFY_SUCCEEDED(indefiniteStopResult);
        VERIFY_SUCCEEDED(immediateStopResult);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }

    WSLC_TEST_METHOD(ConcurrentContainerStopAndStart)
    {
        auto container = LaunchContainerWithBlockingStopHandler("test-concurrent-stop-start");
        auto initProcess = container.GetInitProcess();
        auto input = initProcess.GetStdHandle(0);
        auto outputHandle = initProcess.GetStdHandle(1);
        PartialHandleRead output{outputHandle.Get()};
        output.ExpectConsume("ready\n");

        HRESULT stopResult{};
        HRESULT startResult{};
        std::thread stopThread;
        std::thread startThread;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            input.Reset();

            if (stopThread.joinable())
            {
                stopThread.join();
            }

            if (startThread.joinable())
            {
                startThread.join();
            }
        });

        stopThread = std::thread([&]() { stopResult = container.Get().Stop(WSLCSignalNone, WSLC_STOP_TIMEOUT_NONE); });

        output.ExpectConsume("stopping\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 100), WAIT_TIMEOUT);

        startThread = std::thread([&]() { startResult = container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr); });

        VERIFY_ARE_EQUAL(WaitForSingleObject(startThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        startThread.join();
        VERIFY_ARE_EQUAL(startResult, WSLC_E_CONTAINER_IS_RUNNING);
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 100), WAIT_TIMEOUT);

        const char stopInput = '\n';
        DWORD bytesWritten{};
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(input.Get(), &stopInput, sizeof(stopInput), &bytesWritten, nullptr));
        VERIFY_ARE_EQUAL(bytesWritten, static_cast<DWORD>(sizeof(stopInput)));
        input.Reset();

        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        stopThread.join();
        cleanup.release();

        VERIFY_SUCCEEDED(stopResult);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }

    WSLC_TEST_METHOD(ConcurrentContainerStopAndForceDelete)
    {
        auto container = LaunchContainerWithBlockingStopHandler("test-concurrent-stop-delete");
        auto initProcess = container.GetInitProcess();
        auto input = initProcess.GetStdHandle(0);
        auto outputHandle = initProcess.GetStdHandle(1);
        PartialHandleRead output{outputHandle.Get()};
        output.ExpectConsume("ready\n");

        HRESULT stopResult{};
        HRESULT deleteResult{};
        std::thread stopThread;
        std::thread deleteThread;

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            input.Reset();

            if (stopThread.joinable())
            {
                stopThread.join();
            }

            if (deleteThread.joinable())
            {
                deleteThread.join();
            }
        });

        stopThread = std::thread([&]() { stopResult = container.Get().Stop(WSLCSignalNone, WSLC_STOP_TIMEOUT_NONE); });

        output.ExpectConsume("stopping\n");
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 100), WAIT_TIMEOUT);

        deleteThread = std::thread([&]() { deleteResult = container.Get().Delete(WSLCDeleteFlagsForce); });

        VERIFY_ARE_EQUAL(WaitForSingleObject(deleteThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);
        VERIFY_ARE_EQUAL(WaitForSingleObject(stopThread.native_handle(), 30 * 1000), WAIT_OBJECT_0);

        deleteThread.join();
        stopThread.join();
        input.Reset();
        cleanup.release();

        VERIFY_SUCCEEDED(stopResult);
        VERIFY_SUCCEEDED(deleteResult);
        container.SetDeleteOnClose(false);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateDeleted);
    }

    WSLC_TEST_METHOD(ForceDeleteAutoRemoveContainer)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-force-delete-auto-remove", {"sleep", "99999"});
        launcher.SetContainerFlags(WSLCContainerFlagsRm);

        auto container = launcher.Launch(*m_defaultSession);
        auto id = container.Id();
        auto name = container.Name();

        VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsForce));
        container.SetDeleteOnClose(false);

        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateDeleted);
        VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsForce), RPC_E_DISCONNECTED);

        wil::com_ptr<IWSLCContainer> openedContainer;
        VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(id.c_str(), &openedContainer), WSLC_E_CONTAINER_NOT_FOUND);
        VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(name.c_str(), &openedContainer), WSLC_E_CONTAINER_NOT_FOUND);
    }

    WSLC_TEST_METHOD(ContainerListFilter)
    {
        // Lists containers with the given filter options and returns the names as a set.
        auto listContainers = [&](DWORD flags, std::initializer_list<std::pair<std::string, std::string>> filterPairs) {
            std::vector<std::pair<std::string, std::string>> storage(filterPairs.begin(), filterPairs.end());
            std::vector<WSLCFilter> filters;
            filters.reserve(storage.size());
            for (const auto& [k, v] : storage)
            {
                filters.push_back({.Key = k.c_str(), .Value = v.c_str()});
            }

            WSLCListContainersOptions options{};
            options.Flags = flags;
            options.Filters = filters.data();
            options.FiltersCount = static_cast<ULONG>(filters.size());

            wsl::windows::common::wslc::unique_container_entry_array containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                &options, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

            std::set<std::string> names;
            for (const auto& c : containers)
            {
                names.insert(c.Name);
            }
            return names;
        };

        auto expectContainers =
            [&](DWORD flags, std::initializer_list<std::pair<std::string, std::string>> filterPairs, std::set<std::string> expected) {
                VERIFY_ARE_EQUAL(expected, listContainers(flags, filterPairs));
            };

        // Set up: one running container, one exited container, one created container.
        WSLCContainerLauncher runningLauncher("debian:latest", "filter-running", {"sleep", "99999"});
        runningLauncher.AddLabel("filter.test", "yes");
        runningLauncher.AddLabel("filter.role", "primary");
        auto runningContainer = runningLauncher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(runningContainer.State(), WslcContainerStateRunning);
        std::string runningId = runningContainer.Id();

        WSLCContainerLauncher exitedLauncher("debian:latest", "filter-exited", {"true"});
        exitedLauncher.AddLabel("filter.test", "yes");
        auto exitedContainer = exitedLauncher.Launch(*m_defaultSession);
        exitedContainer.GetInitProcess().Wait();
        VERIFY_ARE_EQUAL(exitedContainer.State(), WslcContainerStateExited);

        WSLCContainerLauncher createdLauncher("debian:latest", "filter-created", {"echo", "hi"});
        auto createdContainer = createdLauncher.Create(*m_defaultSession);
        VERIFY_ARE_EQUAL(createdContainer.State(), WslcContainerStateCreated);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(runningContainer.Get().Delete(WSLCDeleteFlagsForce));
            LOG_IF_FAILED(exitedContainer.Get().Delete(WSLCDeleteFlagsForce));
            LOG_IF_FAILED(createdContainer.Get().Delete(WSLCDeleteFlagsForce));
        });

        // Default (Flags=None, no filters) -> only running containers visible.
        expectContainers(WSLCListContainersFlagsNone, {}, {"filter-running"});

        // --all (Flags=All, no filters) -> all three visible.
        expectContainers(WSLCListContainersFlagsAll, {}, {"filter-running", "filter-exited", "filter-created"});

        // status=exited
        expectContainers(WSLCListContainersFlagsAll, {{"status", "exited"}}, {"filter-exited"});

        // status=running OR status=created (multiple values for the same key are OR'd by Docker).
        expectContainers(
            WSLCListContainersFlagsAll, {{"status", "running"}, {"status", "created"}}, {"filter-running", "filter-created"});

        // name=filter-running
        expectContainers(WSLCListContainersFlagsAll, {{"name", "filter-running"}}, {"filter-running"});

        // id prefix match
        expectContainers(WSLCListContainersFlagsAll, {{"id", runningId.substr(0, 12)}}, {"filter-running"});

        // label=filter.test (key-only) matches running and exited (both have the label).
        expectContainers(WSLCListContainersFlagsAll, {{"label", "filter.test"}}, {"filter-running", "filter-exited"});

        // label=filter.role=primary (key=value) matches only the running container.
        expectContainers(WSLCListContainersFlagsAll, {{"label", "filter.role=primary"}}, {"filter-running"});

        // Multiple keys are AND'd: status=exited AND label=filter.test.
        expectContainers(WSLCListContainersFlagsAll, {{"status", "exited"}, {"label", "filter.test"}}, {"filter-exited"});

        // before=filter-exited -> only containers created before filter-exited are visible.
        expectContainers(WSLCListContainersFlagsAll, {{"before", "filter-exited"}}, {"filter-running"});

        // since=filter-running -> only containers created after filter-running are visible.
        expectContainers(WSLCListContainersFlagsAll, {{"since", "filter-running"}}, {"filter-exited", "filter-created"});

        // exited=0 -> only the exited container that completed successfully.
        expectContainers(WSLCListContainersFlagsAll, {{"exited", "0"}}, {"filter-exited"});

        // Limit caps the result count.
        {
            WSLCListContainersOptions options{};
            options.Flags = WSLCListContainersFlagsAll;
            options.Limit = 1;

            wsl::windows::common::wslc::unique_container_entry_array containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                &options, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

            // Docker returns at most one container; we intersect with the
            // session list so the actual count should also be at most one.
            VERIFY_IS_TRUE(containers.size() <= 1u);
        }
    }

    WSLC_TEST_METHOD(ContainerListDeleteStressTest)
    {
        constexpr auto c_iterations = 50;

        const std::string containerName = "wslc-list-delete-stress";

        std::atomic<unsigned int> failures = 0;

        // One thread repeatedly creates a container and then deletes it.
        std::thread thread([&]() {
            for (unsigned int i = 0; i < c_iterations; ++i)
            {
                WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"});

                auto [hrCreate, container] = launcher.CreateNoThrow(*m_defaultSession);
                if (FAILED(hrCreate))
                {
                    LogError("CreateContainer(%hs) unexpected HR: 0x%08x", containerName.c_str(), hrCreate);
                    ++failures;
                    continue;
                }

                if (i % 2 == 0)
                {
                    auto result = container->Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
                    if (FAILED(result))
                    {
                        LogError("Start(%hs) failed: 0x%08x", containerName.c_str(), result);
                        ++failures;
                    }

                    if (i % 4 == 0)
                    {
                        result = container->Get().Stop(WSLCSignalSIGKILL, 0);
                        if (FAILED(result))
                        {
                            LogError("Stop(%hs) failed: 0x%08x", containerName.c_str(), result);
                            ++failures;
                        }
                    }
                }

                HRESULT result = container->Get().Delete(WSLCDeleteFlagsForce);
                if (FAILED(result))
                {
                    LogError("Delete(%hs) failed: 0x%08x", containerName.c_str(), result);
                    ++failures;
                }
                else
                {
                    container->SetDeleteOnClose(false);
                }
            }
        });

        while (WaitForSingleObject(thread.native_handle(), 0) == WAIT_TIMEOUT)
        {
            WSLCListContainersOptions options{};
            options.Flags = WSLCListContainersFlagsAll;

            wsl::windows::common::wslc::unique_container_entry_array containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            HRESULT hrList = m_defaultSession->ListContainers(
                &options, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>());
            if (FAILED(hrList))
            {
                LogError("ListContainers unexpected HR: 0x%08x", hrList);
                ++failures;
            }
        }

        thread.join();

        VERIFY_ARE_EQUAL(failures.load(), 0u);
    }
};
