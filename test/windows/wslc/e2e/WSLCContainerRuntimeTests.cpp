/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerRuntimeTests.cpp

Abstract:

    This file contains test cases for the WSLC container runtime configuration.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerRuntimeTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerRuntimeTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(TtySize)
    {
        constexpr ULONG c_rows = 43;
        constexpr ULONG c_columns = 42;
        const std::string expectedSize = "43 42";

        // Container init process.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "tty-size-init", {"/bin/sh", "-c", "while true; do stty size; sleep 1; done"}, {}, {}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);
            launcher.SetTtySize(c_rows, c_columns);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            auto tty = process.GetStdHandle(WSLCFDTty);

            // Wait for the size to be reflected in a loop, since the tty size is applied asynchronously.
            PartialHandleRead reader(tty.Get());
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() { THROW_HR_IF(E_ABORT, reader.GetData().find(expectedSize) == std::string::npos); },
                std::chrono::milliseconds(100),
                std::chrono::seconds(60));
        }

        // Exec process.
        {
            WSLCContainerLauncher launcher("debian:latest", "tty-size-exec", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);

            WSLCProcessLauncher execLauncher({}, {"/usr/bin/stty", "size"}, {}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);
            execLauncher.SetTtySize(c_rows, c_columns);

            auto process = execLauncher.Launch(container.Get());

            ValidateProcessOutput(process, {{WSLCFDTty, expectedSize + "\r\n"}});
        }
    }

    WSLC_TEST_METHOD(ContainerStats_RunningContainer)
    {
        // Start a long-lived detached container on a bridged network so network stats are populated.
        WSLCContainerLauncher launcher("debian:latest", "wslc-test-stats", {"sleep", "60"}, {}, "bridge");

        auto runningContainer = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

        wil::com_ptr<IWSLCContainer> container;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-test-stats", &container));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(container->Stats(&output));
        VERIFY_IS_NOT_NULL(output.get());
        VERIFY_IS_FALSE(std::string(output.get()).empty());

        const auto stats = wsl::shared::FromJson<wsl::windows::common::docker_schema::ContainerStats>(output.get());

        // cpu_stats
        // The VM has been running so system_cpu_usage is non-zero.
        VERIFY_IS_GREATER_THAN(stats.cpu_stats.system_cpu_usage, 0ull);

        // The container process itself has consumed some CPU.
        VERIFY_IS_GREATER_THAN(stats.cpu_stats.cpu_usage.total_usage, 0ull);

        // Kernel + user time together must not exceed total CPU time.
        VERIFY_IS_LESS_THAN_OR_EQUAL(
            stats.cpu_stats.cpu_usage.usage_in_kernelmode + stats.cpu_stats.cpu_usage.usage_in_usermode, stats.cpu_stats.cpu_usage.total_usage);

        // The session was created with 4 CPUs.
        VERIFY_IS_GREATER_THAN(stats.cpu_stats.online_cpus, 0u);

        // precpu_stats
        // precpu_stats is a prior snapshot; its total must not exceed the current total.
        VERIFY_IS_LESS_THAN_OR_EQUAL(stats.precpu_stats.cpu_usage.total_usage, stats.cpu_stats.cpu_usage.total_usage);
        VERIFY_IS_LESS_THAN_OR_EQUAL(stats.precpu_stats.system_cpu_usage, stats.cpu_stats.system_cpu_usage);

        // memory_stats
        // Limit is the VM memory ceiling — must be non-zero.
        VERIFY_IS_GREATER_THAN(stats.memory_stats.limit, 0ull);

        // The sleep process occupies at least some memory.
        VERIFY_IS_GREATER_THAN(stats.memory_stats.usage, 0ull);

        // Usage must never exceed the reported limit.
        VERIFY_IS_LESS_THAN_OR_EQUAL(stats.memory_stats.usage, stats.memory_stats.limit);

        // pids_stats
        // At minimum the sleep process itself must be counted.
        VERIFY_IS_GREATER_THAN(stats.pids_stats.current, 0ull);

        // networks
        // A bridged container always has at least one network interface.
        VERIFY_IS_TRUE(stats.networks.has_value());
        VERIFY_IS_FALSE(stats.networks->empty());

        // Every interface entry must have consistent packet/byte counts
        // (bytes >= 0 is trivially true for unsigned, but packets imply bytes >= 0 too).
        for (const auto& [iface, net] : *stats.networks)
        {
            VERIFY_IS_FALSE(iface.empty());

            // If packets were received/sent, the byte count must also be non-zero.
            if (net.rx_packets > 0)
            {
                VERIFY_IS_GREATER_THAN(net.rx_bytes, 0ull);
            }
            if (net.tx_packets > 0)
            {
                VERIFY_IS_GREATER_THAN(net.tx_bytes, 0ull);
            }
        }

        // blkio_stats
        // io_service_bytes_recursive may be absent for a container with no disk I/O,
        // but if present every entry must have a non-empty operation name.
        if (stats.blkio_stats.io_service_bytes_recursive.has_value())
        {
            for (const auto& entry : *stats.blkio_stats.io_service_bytes_recursive)
            {
                VERIFY_IS_FALSE(entry.op.empty());
            }
        }
    }

    WSLC_TEST_METHOD(ContainerStats_NullOutputPointer)
    {
        WSLCContainerLauncher launcher("debian:latest", "wslc-test-stats-null", {"sleep", "60"}, {}, "bridge");
        auto runningContainer = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

        wil::com_ptr<IWSLCContainer> container;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-test-stats-null", &container));

        // Passing nullptr for Output must fail.
        VERIFY_FAILED(container->Stats(nullptr));
    }

    WSLC_TEST_METHOD(ContainerStats_CreatedContainer_ReturnsZeroedStats)
    {
        // A created-but-not-started container returns zeroed stats from Docker rather than an error.
        WSLCContainerLauncher launcher("debian:latest", "wslc-test-stats-created", {}, {}, "bridge");
        auto [result, runningContainer] = launcher.CreateNoThrow(*m_defaultSession);
        VERIFY_SUCCEEDED(result);

        wil::com_ptr<IWSLCContainer> container;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer("wslc-test-stats-created", &container));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(container->Delete(WSLCDeleteFlagsForce)); });

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(container->Stats(&output));
        VERIFY_IS_NOT_NULL(output.get());

        // A non-running container has no active processes.
        auto stats = wsl::shared::FromJson<wsl::windows::common::docker_schema::ContainerStats>(output.get());
        VERIFY_ARE_EQUAL(0ull, stats.pids_stats.current);
    }

    WSLC_TEST_METHOD(InvalidNames)
    {
        auto expectInvalidArg = [&](const std::string& name) {
            wil::com_ptr<IWSLCContainer> container;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(name.c_str(), &container), E_INVALIDARG);
            VERIFY_IS_NULL(container.get());

            ValidateCOMErrorMessage(std::format(L"Invalid name: '{}'", name));
        };

        expectInvalidArg("container with spaces");
        expectInvalidArg("?foo");
        expectInvalidArg("?foo&bar");
        expectInvalidArg("/url/path");
        expectInvalidArg("");
        expectInvalidArg("\\escaped\n\\chars");

        std::string longName(WSLC_MAX_CONTAINER_NAME_LENGTH + 1, 'a');
        expectInvalidArg(longName);

        auto expectInvalidPull = [&](const char* name) {
            VERIFY_ARE_EQUAL(m_defaultSession->PullImage(name, nullptr, FALSE, nullptr, nullptr), E_INVALIDARG);

            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            VERIFY_ARE_EQUAL(comError->Message.get(), std::format(L"Invalid image: '{}'", name));
        };

        expectInvalidPull("?foo&bar/url\n:name");
        expectInvalidPull("?:&");
        expectInvalidPull("/:/");
        expectInvalidPull("\n: ");
        expectInvalidPull("invalid\nrepo:valid-image");
        expectInvalidPull("bad!repo:valid-image");
        expectInvalidPull("repo:badimage!name");
        expectInvalidPull("bad+image");
    }

    WSLC_TEST_METHOD(PageReporting)
    {
        SKIP_TEST_ARM64();

        // Determine expected page reporting order based on Windows version.
        // On Germanium or later: 5 (128k), otherwise: 9 (2MB).
        const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
        int expectedOrder = (windowsVersion.BuildNumber >= wsl::windows::common::helpers::WindowsBuildNumbers::Germanium) ? 5 : 9;

        // Read the actual value from sysfs and verify it matches.
        auto result =
            ExpectCommandResult(m_defaultSession.get(), {"/bin/cat", "/sys/module/page_reporting/parameters/page_reporting_order"}, 0);

        VERIFY_ARE_EQUAL(result.Output[1], std::format("{}\n", expectedOrder));
    }

    WSLC_TEST_METHOD(SwapConfigured)
    {
        // Swap is configured asynchronously (mkswap + swapon runs fire-and-forget), so retry until it's active.
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                auto result = ExpectCommandResult(m_defaultSession.get(), {"/usr/sbin/swapon", "--show=NAME,SIZE", "--noheadings"}, 0);

                THROW_WIN32_IF(ERROR_RETRY, result.Code != 0 || result.Output.size() < 2 || result.Output[1].find("/dev/") == std::string::npos);
            },
            std::chrono::milliseconds{500},
            std::chrono::seconds{30});
    }

    WSLC_TEST_METHOD(ContainerAutoRemove)
    {
        // Test that a container with the Rm flag is automatically deleted on Stop().
        {
            WSLCContainerLauncher launcher("debian:latest", "test-auto-remove", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Test that a container with the Rm flag is automatically deleted when the init process is killed.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-auto-remove", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            // Prevent container from being deleted when handle is closed so we can verify auto-remove behavior.
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(process.Get().Signal(WSLCSignalSIGKILL));
            process.Wait();

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Test that a container with the Rm flag is automatically deleted when the container is killed.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-auto-remove-kill", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            // Prevent container from being deleted when handle is closed so we can verify auto-remove behavior.
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
            process.Wait();

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove-kill", &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Test that the container autoremove flag is applied when the container exits on its own.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-hostname", {"/bin/sh", "-c", "echo foo"});
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            process.Wait();

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Test that the Rm flag is persisted across wslc sessions.
        {
            {
                WSLCContainerLauncher launcher("debian:latest", "test-auto-remove", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
                launcher.SetContainerFlags(WSLCContainerFlagsRm);

                auto container = launcher.Create(*m_defaultSession);
                container.SetDeleteOnClose(false);

                ResetTestSession();
            }

            auto container = OpenContainer(m_defaultSession.get(), "test-auto-remove");
            auto id = container.Id();

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            // verifyContainerDeleted("test-auto-remove");
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), WSLC_E_CONTAINER_NOT_FOUND);
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(id.c_str(), &notFound), WSLC_E_CONTAINER_NOT_FOUND);

            wsl::windows::common::wslc::unique_container_entry_array containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
            VERIFY_ARE_EQUAL(containers.size(), 0);
        }
    }

    WSLC_TEST_METHOD(ContainerAutoRemoveReadStdout)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-auto-remove-stdout", {"echo", "Hello World"});
        launcher.SetContainerFlags(WSLCContainerFlagsRm);

        auto container = launcher.Launch(*m_defaultSession);

        // Wait for the container to exit and verify it gets deleted automatically.
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_WIN32_IF(ERROR_RETRY, container.State() != WslcContainerStateDeleted); },
            std::chrono::milliseconds{100},
            std::chrono::seconds{30});

        VERIFY_ARE_EQUAL(WslcContainerStateDeleted, container.State());
        VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

        // Ensure we can still get the init process and read stdout.
        auto process = container.GetInitProcess();
        auto result = process.WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_ARE_EQUAL(std::string("Hello World\n"), result.Output[1]);

        // Validate that the container is not found if we try to open it by name or id, or found in the container list.
        wil::com_ptr<IWSLCContainer> notFound;
        VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove-stdout", &notFound), WSLC_E_CONTAINER_NOT_FOUND);

        wsl::windows::common::wslc::unique_container_entry_array containers;
        wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
        VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
            nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(containers.size(), 0);
    }

    WSLC_TEST_METHOD(ContainerNameGeneration)
    {
        {
            // Create a container with a specific name.
            auto container = WSLCContainerLauncher("debian:latest", "test-container-name").Create(*m_defaultSession.get());

            // Validate that the container name is correct.
            VERIFY_ARE_EQUAL(container.Name(), "test-container-name");
        }

        {
            // Create a container without name.
            auto container = WSLCContainerLauncher("debian:latest").Create(*m_defaultSession.get());

            // Validate that the service generates a name in the format "descriptor_mountain[digit]".
            auto name = container.Name();
            VERIFY_ARE_NOT_EQUAL(name, "");

            auto underscore = name.find('_');
            VERIFY_ARE_NOT_EQUAL(underscore, std::string::npos);

            auto descriptor = name.substr(0, underscore);
            auto mountain = name.substr(underscore + 1);

            // Strip trailing retry digit if present.
            if (!mountain.empty() && std::isdigit(mountain.back()))
            {
                mountain.pop_back();
            }

            using wsl::windows::service::wslc::c_descriptors;
            using wsl::windows::service::wslc::c_mountains;

            VERIFY_IS_TRUE(std::ranges::find(c_descriptors, descriptor) != c_descriptors.end());
            VERIFY_IS_TRUE(std::ranges::find(c_mountains, mountain) != c_mountains.end());
        }

        {
            // Create multiple containers without names and verify they get unique names.
            auto container1 = WSLCContainerLauncher("debian:latest").Create(*m_defaultSession.get());
            auto container2 = WSLCContainerLauncher("debian:latest").Create(*m_defaultSession.get());
            auto container3 = WSLCContainerLauncher("debian:latest").Create(*m_defaultSession.get());

            VERIFY_ARE_NOT_EQUAL(container1.Name(), container2.Name());
            VERIFY_ARE_NOT_EQUAL(container1.Name(), container3.Name());
            VERIFY_ARE_NOT_EQUAL(container2.Name(), container3.Name());
        }
    }

    WSLC_TEST_METHOD(DeferredPortAndVolumeMappingOnStart)
    {
        // Verify port mapping.
        // Two containers created with the same host port, only the first Start() succeeds.
        {
            WSLCContainerLauncher launcher("debian:latest", "deferred-port", {"sleep", "99999"}, {}, "bridge");
            launcher.AddPort(1240, 8000, AF_INET);

            // Both Create() calls should succeed because ports are not reserved until Start().
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);

            launcher.SetName("deferred-port-2");
            auto container2 = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container2.State(), WslcContainerStateCreated);

            // Start container — should succeed.
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Start container 2 — should fail because the host port is already reserved by container 1.
            VERIFY_ARE_EQUAL(container2.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), HRESULT_FROM_WIN32(WSAEADDRINUSE));
            VERIFY_ARE_EQUAL(container2.State(), WslcContainerStateCreated);
        }

        // Verify mount volume is deferred to Start()
        {
            auto hostFolder = std::filesystem::current_path() / "test-deferred-volume";
            std::filesystem::create_directories(hostFolder);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                std::error_code ec;
                std::filesystem::remove_all(hostFolder, ec);
            });

            auto getMountCount = [&]() {
                auto result = RunCommand(m_defaultSession.get(), {"/bin/sh", "-c", "findmnt -o TARGET -l | grep -c '^/mnt/'"});
                return std::stoi(result.Output[1]);
            };

            auto baselineMountCount = getMountCount();

            WSLCContainerLauncher launcher("debian:latest", "deferred-volume", {"sleep", "99999"}, {}, "host");
            launcher.AddVolume(hostFolder.wstring(), "/deferred-volume", false);

            // Create the container — volume should NOT be mounted yet.
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateCreated);
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount);

            // Start the container — volume should now be mounted.
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateRunning);
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount + 1);

            // Verify the volume is unmounted after container is stopped.
            VERIFY_SUCCEEDED(container->Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount);
        }
    }

    // This test case validates that multiple operations can happen in parallel in the same session.
    WSLC_TEST_METHOD(ParallelSessionOperations)
    {
        // Start a blocking export
        BlockingOperation operation([&](HANDLE handle) {
            return m_defaultSession->SaveImage(ToCOMInputHandle(handle), "debian:latest", nullptr, nullptr);
        });

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operation.Complete(); });

        // Validate that various operations can be done while the export is in progress.

        {
            wsl::windows::common::wslc::unique_container_entry_array containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

            if (containers.size() > 0)
            {
                LogError("Unexpected container found: %hs", containers[0].Name);
                VERIFY_FAIL();
            }
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "test-parallel-operation", {"echo", "OK"});

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});

            auto containerRef = OpenContainer(m_defaultSession.get(), "test-parallel-operation");
        }

        {
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, &images, images.size_address<ULONG>()));
        }
    }

    WSLC_TEST_METHOD(ParallelContainerOperations)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-parallel-container-operations", {"echo", "OK"});

        auto container = launcher.Launch(*m_defaultSession);

        auto process = container.GetInitProcess();
        ValidateProcessOutput(process, {{1, "OK\n"}});

        // Start a blocking export
        BlockingOperation operation([&](HANDLE handle) { return container.Get().Export(ToCOMInputHandle(handle)); });

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operation.Complete(); });

        // Validate that various operations can be done while the export is in progress.
        {
            VERIFY_ARE_EQUAL(container.GetInitProcess().Wait(), 0);
        }

        {
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
        }

        {
            COMOutputHandle stdoutHandle;
            COMOutputHandle stderrHandle;
            VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsNone, &stdoutHandle, &stderrHandle, 0, 0, false));

            ValidateHandleOutput(stdoutHandle.Get(), "OK\n");
        }

        {
            VERIFY_ARE_EQUAL(container.Inspect().State.Status, "exited");
        }

        {
            VERIFY_ARE_EQUAL(container.Labels().size(), 0);
        }

        {
            // Validate that another export can run.
            BlockingOperation secondExport([&](HANDLE handle) { return container.Get().Export(ToCOMInputHandle(handle)); });
            secondExport.Complete();
        }

        {
            // Exec() fails because the container is not running. This call just validates that Exec() doesn't get stuck.
            auto [result, _] = WSLCProcessLauncher({}, {"echo", "OK"}).LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, WSLC_E_CONTAINER_NOT_RUNNING);
        }
    }

    WSLC_TEST_METHOD(SessionTerminationDuringSave)
    {
        // Validate that SaveImage is aborted when the session terminates.
        // Use overlapped write pipe so the server-side WriteFile doesn't block synchronously.
        BlockingOperation operation(
            [&](HANDLE handle) { return m_defaultSession->SaveImage(ToCOMInputHandle(handle), "debian:latest", nullptr, nullptr); }, E_ABORT, true, true);

        // Terminate the session.
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        operation.Complete();
        auto restore = ResetTestSession();
    }

    WSLC_TEST_METHOD(SessionTerminationDuringExport)
    {
        // Validate that container Export is aborted when the session terminates.
        WSLCContainerLauncher launcher("debian:latest", "test-export-session-terminate", {"echo", "OK"});
        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.GetInitProcess().Wait(), 0);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            PruneResult result;
            LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));
        });

        // Use overlapped write pipe so the server-side WriteFile doesn't block synchronously.
        BlockingOperation operation([&](HANDLE handle) { return container.Get().Export(ToCOMInputHandle(handle)); }, E_ABORT, true, true);

        // Avoid attempting container delete on scope exit after intentional session termination;
        // rely on the prune scope-exit above to clean up instead.
        container.SetDeleteOnClose(false);

        // Terminate the session.
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        operation.Complete();
        auto restore = ResetTestSession();
    }

    WSLC_TEST_METHOD(InteractiveDetach)
    {
        auto validateDetaches = [](HANDLE TtyIn, HANDLE TtyOut, const std::vector<char>& Input) {
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(TtyIn, Input.data(), static_cast<DWORD>(Input.size()), nullptr, nullptr));

            std::string output;
            auto onRead = [&](const gsl::span<char>& data) { output.append(data.data(), data.size()); };

            wsl::windows::common::io::MultiHandleWait io;
            io.AddHandle(std::make_unique<wsl::windows::common::io::ReadHandle>(TtyOut, std::move(onRead)));

            io.Run(60s);

            // N.B. In the case of exec, the output can either be 'read escape sequence' or 'exec attach failed [...]' based on timing.
            std::set<std::string> expectedOutputs{
                "", "\r\n", "exec attach failed: error on attach stdin: read escape sequence\r\n", "read escape sequence\r\n"};

            if (expectedOutputs.find(output) == expectedOutputs.end())
            {
                LogError("Unexpected output: %hs", output.c_str());
                VERIFY_FAIL();
            }
        };

        auto runDetachTest = [&](LPCSTR DetachKeys, const std::vector<char>& DetachSequence) {
            WSLCContainerLauncher launcher("debian:latest", "test-detach", {"sleep", "9999999"}, {}, {}, WSLCProcessFlagsStdin | WSLCProcessFlagsTty);

            auto container = launcher.Create(*m_defaultSession);

            WSLCProcessStartOptions startOptions{};
            startOptions.TtyRows = 24;
            startOptions.TtyColumns = 80;
            startOptions.DetachKeys = DetachKeys;
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, &startOptions, nullptr));

            auto initProcess = container.GetInitProcess();

            // Validate detaching from a started container with the attach flag.
            {
                auto tty = initProcess.GetStdHandle(WSLCFDTty);
                validateDetaches(tty.Get(), tty.Get(), DetachSequence);
            }

            // Validate detaching from an attached tty.
            {
                COMOutputHandle ttyHandle{};
                COMOutputHandle unusedHandle1{};
                COMOutputHandle unusedHandle2{};
                VERIFY_SUCCEEDED(container.Get().Attach(DetachKeys, &ttyHandle, &unusedHandle1, &unusedHandle2));

                validateDetaches(ttyHandle.Get(), ttyHandle.Get(), DetachSequence);
            }

            // Validate detaching from an exec'd process.
            {
                WSLCProcessLauncher processLauncher({}, {"sleep", "9999999"}, {}, WSLCProcessFlagsStdin | WSLCProcessFlagsTty);

                if (DetachKeys != nullptr)
                {
                    processLauncher.SetDetachKeys(DetachKeys);
                }

                auto process = processLauncher.Launch(container.Get());
                auto tty = process.GetStdHandle(WSLCFDTty);

                validateDetaches(tty.Get(), tty.Get(), DetachSequence);
            }
        };

        {
            // Validate that by default ttys can be detached via ctrlp-ctrlq.
            runDetachTest(nullptr, {0x10, 0x11});

            // Validate other detach keys.
            runDetachTest("ctrl-a", {0x1});
            runDetachTest("a,b,c,d,ctrl-z", {'a', 'b', 'c', 'd', 0x1a});
        }

        {
            // Validate that invalid detach keys fail with the appropriate error.
            // N.B. Docker doesn't set an error message for this specific case.
            WSLCContainerLauncher launcher("debian:latest", "test-detach", {"cat"}, {}, {}, WSLCProcessFlagsStdin | WSLCProcessFlagsTty);
            auto container = launcher.Create(*m_defaultSession);

            WSLCProcessStartOptions invalidDetachOptions{};
            invalidDetachOptions.TtyRows = 24;
            invalidDetachOptions.TtyColumns = 80;
            invalidDetachOptions.DetachKeys = "invalid";
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsAttach, &invalidDetachOptions, nullptr), E_INVALIDARG);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));

            COMOutputHandle unusedHandle{};
            VERIFY_ARE_EQUAL(container.Get().Attach("invalid", &unusedHandle, &unusedHandle, &unusedHandle), E_INVALIDARG);

            WSLCProcessLauncher processLauncher({}, {"cat"}, {}, WSLCProcessFlagsStdin | WSLCProcessFlagsTty);
            processLauncher.SetDetachKeys("invalid");

            // N.B. Docker returns HTTP 500 if the detach keys are invalid, but unlike other cases there's a proper error message.
            auto [result, _] = processLauncher.LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, E_FAIL);

            ValidateCOMErrorMessage(L"Invalid escape keys (invalid) provided");
        }
    }

    WSLC_TEST_METHOD(ContainerPrune)
    {
        auto expectPrune = [this](
                               const std::vector<std::string>& expectedIds = {},
                               const std::vector<std::pair<std::string, std::string>>& filterPairs = {},
                               const std::source_location& source = std::source_location::current()) {
            PruneResult result;

            std::vector<WSLCFilter> filters;
            filters.reserve(filterPairs.size());
            for (const auto& [key, value] : filterPairs)
            {
                filters.push_back({key.c_str(), value.c_str()});
            }

            VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(
                filters.empty() ? nullptr : filters.data(), static_cast<ULONG>(filters.size()), &result.result));

            std::vector<std::string> prunedContainers;
            for (size_t i = 0; i < result.result.ContainersCount; i++)
            {
                prunedContainers.push_back(result.result.Containers[i]);
            }

            VerifyAreEqualUnordered(expectedIds, prunedContainers, source);
        };

        auto RunAndWait = [&](auto& launcher) {
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OK\n"}});

            return container;
        };

        // Validate that a prune without any container returns nothing.
        {
            expectPrune({});
        }

        {
            // Validate that prune doesn't remove running containers.
            WSLCContainerLauncher launcher("debian:latest", "test-prune", {"sleep", "9999999"}, {}, {});
            auto container = launcher.Launch(*m_defaultSession);

            expectPrune({});

            // Validate that prune removes stopped containers.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            auto containerId = container.Id();
            expectPrune({containerId});

            // Validate that the container can't be opened anymore.
            wil::com_ptr<IWSLCContainer> dummy;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(containerId.c_str(), &dummy), WSLC_E_CONTAINER_NOT_FOUND);

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);
        }

        // Validate that label filters work.
        {
            WSLCContainerLauncher testPrune1Launcher("debian:latest", "test-prune-1", {"echo", "OK"}, {}, {});
            testPrune1Launcher.AddLabel("key", "value");

            auto testPrune1 = RunAndWait(testPrune1Launcher);

            WSLCContainerLauncher testPrune2Launcher("debian:latest", "test-prune-2", {"echo", "OK"}, {}, {});
            testPrune2Launcher.AddLabel("key", "anotherValue");

            auto testPrune2 = RunAndWait(testPrune2Launcher);

            WSLCContainerLauncher testPrune3Launcher("debian:latest", "test-prune-3", {"echo", "OK"}, {}, {});
            testPrune3Launcher.AddLabel("anotherKey", "value");
            auto testPrune3 = RunAndWait(testPrune3Launcher);

            WSLCContainerLauncher testPrune4Launcher("debian:latest", "test-prune-4", {"echo", "OK"}, {}, {});
            auto testPrune4 = RunAndWait(testPrune4Launcher);

            // Expect testPrune1 to be selected via key=value.
            expectPrune({testPrune1.Id()}, {{"label", "key=value"}});

            // Expect testPrune2 to be selected via key being present.
            expectPrune({testPrune2.Id()}, {{"label", "key"}});

            // Prune by absence of 'anotherKey' label.
            expectPrune({testPrune4.Id()}, {{"label!", "anotherKey"}});

            // Prune by label inequality.
            expectPrune({testPrune3.Id()}, {{"label!", "anotherKey=someValue"}});
        }

        // Validate that the 'until' filter works.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-prune-until", {"echo", "OK"}, {}, {});

            auto container = RunAndWait(launcher);

            auto now = time(nullptr);

            expectPrune({}, {{"until", std::to_string(now - 3600)}});
            expectPrune({container.Id()}, {{"until", std::to_string(now + 3600)}});
        }

        // Validate error paths.
        {
            WSLCFilter filter{.Key = nullptr, .Value = nullptr};
            PruneResult result;

            VERIFY_ARE_EQUAL(m_defaultSession->PruneContainers(&filter, 1, &result.result), E_POINTER);
            VERIFY_ARE_EQUAL(m_defaultSession->PruneContainers(&filter, 1, nullptr), HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));
        }
    }

    WSLC_TEST_METHOD(ImagePrune)
    {
        auto pruneImages = [this](const std::vector<WSLCFilter>& filters = {}) {
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            ULONGLONG spaceReclaimed = 0;

            VERIFY_SUCCEEDED(m_defaultSession->PruneImages(
                filters.empty() ? nullptr : filters.data(),
                static_cast<ULONG>(filters.size()),
                deletedImages.addressof(),
                deletedImages.size_address<ULONG>(),
                &spaceReclaimed));
            return std::make_pair(std::move(deletedImages), spaceReclaimed);
        };

        // Helper to create a dangling image using only test-local tags:
        // Load alpine and hello-world under unique tags, then overwrite one with the other.
        auto createDanglingImage = [this]() {
            LoadTestImage(*m_defaultSession, "alpine:latest");
            WSLCTagImageOptions tagA{.Image = "alpine:latest", .Repo = "prune-test-a", .Tag = "v1"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagA));
            DeleteImage("alpine:latest", WSLCDeleteImageFlagsNone);

            LoadTestImage(*m_defaultSession, "hello-world:latest");
            WSLCTagImageOptions tagB{.Image = "hello-world:latest", .Repo = "prune-test-b", .Tag = "v1"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagB));
            DeleteImage("hello-world:latest", WSLCDeleteImageFlagsNone);

            // Overwrite prune-test-a with prune-test-b's image, making original alpine dangling.
            WSLCTagImageOptions overwrite{.Image = "prune-test-b:v1", .Repo = "prune-test-a", .Tag = "v1"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&overwrite));
        };

        auto cleanupDanglingImage = [this, &pruneImages]() {
            pruneImages({{.Key = "dangling", .Value = "true"}});
            LOG_IF_FAILED(DeleteImageNoThrow("prune-test-a:v1", WSLCDeleteImageFlagsNone).first);
            LOG_IF_FAILED(DeleteImageNoThrow("prune-test-b:v1", WSLCDeleteImageFlagsNone).first);
        };

        // Clean up any stale dangling images from prior tests.
        pruneImages({{.Key = "dangling", .Value = "true"}});

        // Prune with no unused images returns empty.
        {
            auto [deletedImages, spaceReclaimed] = pruneImages();
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);
        }

        // Validate dangling prune: create a dangling image by re-tagging, then prune it.
        {
            createDanglingImage();
            auto cleanup = wil::scope_exit([&]() { cleanupDanglingImage(); });

            // dangling=true should prune the now-dangling original alpine image.
            auto [deletedImages, spaceReclaimed] = pruneImages({{.Key = "dangling", .Value = "true"}});
            VERIFY_IS_TRUE(deletedImages.size() > 0);

            // A second prune should find nothing.
            auto [deletedImages2, spaceReclaimed2] = pruneImages({{.Key = "dangling", .Value = "true"}});
            VERIFY_ARE_EQUAL(deletedImages2.size(), 0u);
        }

        // Validate 'until' filter.
        {
            createDanglingImage();
            auto cleanup = wil::scope_exit([&]() { cleanupDanglingImage(); });

            // Docker's 'until' filter uses the image's original Created timestamp, not load time.
            // Use timestamp 1 (near epoch) which is before any real image was built.
            auto [deletedImages, spaceReclaimed] = pruneImages({{.Key = "until", .Value = "1"}});
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);

            // Use a timestamp far in the future to ensure the dangling image is pruned.
            auto futureStr = std::to_string(static_cast<uint64_t>(time(nullptr)) + 3600);
            auto [deletedImages2, spaceReclaimed2] = pruneImages({{.Key = "until", .Value = futureStr.c_str()}});
            VERIFY_IS_TRUE(deletedImages2.size() > 0);
        }

        // Validate label filters.
        {
            createDanglingImage();
            auto cleanup = wil::scope_exit([&]() { cleanupDanglingImage(); });

            // Prune with a label filter that no dangling image has - should not prune anything.
            auto [deletedImages, spaceReclaimed] = pruneImages({{.Key = "label", .Value = "nonexistent.label"}});
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);

            // Prune with absent label filter ("label!") - dangling image doesn't have the label, so it matches.
            auto [deletedImages2, spaceReclaimed2] = pruneImages({{.Key = "label!", .Value = "nonexistent.label"}});
            VERIFY_IS_TRUE(deletedImages2.size() > 0);
        }

        // Validate null Filters uses defaults (dangling-only prune).
        {
            LoadTestImage(*m_defaultSession, "alpine:latest");
            WSLCTagImageOptions renameOptions{.Image = "alpine:latest", .Repo = "prune-test-a", .Tag = "v1"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&renameOptions));
            DeleteImage("alpine:latest", WSLCDeleteImageFlagsNone);
            auto cleanup = wil::scope_exit([&]() { cleanupDanglingImage(); });

            ExpectImagePresent(*m_defaultSession, "prune-test-a:v1");

            // Null filters should not prune tagged images (docker defaults to dangling-only).
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            ULONGLONG spaceReclaimed = 0;
            VERIFY_SUCCEEDED(m_defaultSession->PruneImages(
                nullptr, 0, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed));
            ExpectImagePresent(*m_defaultSession, "prune-test-a:v1");
        }

        // Validate error paths.
        {
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            ULONGLONG spaceReclaimed = 0;

            // Null output pointers - RPC rejects null [out] pointers before our code runs.
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(nullptr, 0, nullptr, deletedImages.size_address<ULONG>(), &spaceReclaimed),
                HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));

            // Unknown filter key - docker rejects with HTTP 400, mapped to E_INVALIDARG.
            WSLCFilter bogus{.Key = "bogus", .Value = "x"};
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(&bogus, 1, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed),
                E_INVALIDARG);
            ValidateCOMErrorMessageContains(L"invalid filter 'bogus'");

            // Null filter key - rejected by ParseKeyMultiValuePairs at the boundary.
            WSLCFilter nullKey{.Key = nullptr, .Value = "x"};
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(&nullKey, 1, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed),
                E_POINTER);
        }
    }
};
