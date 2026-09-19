/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerInspectExecTests.cpp

Abstract:

    This file contains test cases for the WSLC container inspect and exec API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerInspectExecTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerInspectExecTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(ContainerInspect)
    {
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

                    // WSLC always binds to localhost.
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
        auto expectMounts = [&](const auto& actualMounts,
                                const std::vector<std::tuple<std::string, std::string, std::optional<std::filesystem::path>, bool>>& expectedMounts) {
            VERIFY_ARE_EQUAL(actualMounts.size(), expectedMounts.size());

            for (const auto& [expectedDest, expectedType, expectedSource, expectedReadWrite] : expectedMounts)
            {
                auto it = std::ranges::find_if(actualMounts, [&](const auto& mount) { return mount.Destination == expectedDest; });
                if (it == actualMounts.end())
                {
                    LogError("Expected mount destination not found: %hs", expectedDest.c_str());
                    VERIFY_FAIL();
                }

                VERIFY_IS_FALSE(it->Type.empty());
                VERIFY_ARE_EQUAL(it->Type, expectedType);

                if (expectedSource.has_value())
                {
                    if (expectedType == "bind")
                    {
                        const std::filesystem::path actualSource(it->Source);
                        VERIFY_IS_TRUE(actualSource.is_absolute());
                        VERIFY_IS_TRUE(std::filesystem::equivalent(actualSource, expectedSource.value()));
                    }
                    else
                    {
                        VERIFY_ARE_EQUAL(it->Source, expectedSource->string());
                    }
                }
                else
                {
                    VERIFY_IS_TRUE(it->Source.empty());
                }
                VERIFY_ARE_EQUAL(it->ReadWrite, expectedReadWrite);
            }
        };

        // Test a running container with port mappings and volumes.
        {
            auto testFolder = std::filesystem::current_path() / "test-inspect-volume";
            auto testFolderReadOnly = std::filesystem::current_path() / "test-inspect-volume-ro";
            const std::string guestVolumeName = "test-container-inspect-guest-volume";

            std::filesystem::create_directories(testFolder);
            std::filesystem::create_directories(testFolderReadOnly);

            auto cleanup = wil::scope_exit([&]() {
                std::error_code ec;
                std::filesystem::remove_all(testFolder, ec);
                std::filesystem::remove_all(testFolderReadOnly, ec);
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(guestVolumeName.c_str()));
            });

            CreateNamedVolume(guestVolumeName, "guest");

            WSLCContainerLauncher launcher("debian:latest", "test-container-inspect", {"sleep", "99999"}, {}, "bridge");

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1235, 8000, AF_INET);
            launcher.AddPort(1236, 8001, AF_INET);
            launcher.AddVolume(testFolder.wstring(), "/test-volume", false);
            launcher.AddVolume(testFolderReadOnly.wstring(), "/test-volume-ro", true);
            launcher.AddNamedVolume(guestVolumeName, "/test-guest-volume", false);
            launcher.AddTmpfs("/mnt/wslc-tmpfs-inspect", "");

            auto container = launcher.Launch(*m_defaultSession);

            // Validate that inspect fails with a null pointer.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), container.Get().Inspect(FALSE, nullptr));

            auto details = container.Inspect();

            // Verify basic container metadata.
            VERIFY_IS_FALSE(details.Id.empty());
            VERIFY_ARE_EQUAL(details.Name, "/test-container-inspect");
            VERIFY_IS_TRUE(details.Image.starts_with("sha256:"));
            VERIFY_ARE_EQUAL(details.Config.Image, "debian:latest");
            VERIFY_IS_FALSE(details.Created.empty());

            // Verify container state.
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "bridge");
            VERIFY_IS_TRUE(details.State.Running);
            VERIFY_ARE_EQUAL(details.State.Status, "running");
            VERIFY_IS_FALSE(details.State.StartedAt.empty());

            // Verify port mappings match what we configured.
            expectPorts(details.Ports, {{"8000/tcp", {"1234", "1235"}}, {"8001/tcp", {"1236"}}});

            // Verify mounts match what we configured.
            expectMounts(
                details.Mounts,
                {{"/test-volume", "bind", testFolder, true},
                 {"/test-volume-ro", "bind", testFolderReadOnly, false},
                 {"/test-guest-volume", "volume", std::filesystem::path{guestVolumeName}, true},
                 {"/mnt/wslc-tmpfs-inspect", "tmpfs", std::nullopt, true}});

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Test an exited container still returns correct schema shape.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-inspect-exited", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);

            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK\n"}});

            auto details = container.Inspect();

            // Verify basic container metadata is present.
            VERIFY_IS_FALSE(details.Id.empty());
            VERIFY_ARE_EQUAL(details.Name, "/test-container-inspect-exited");
            VERIFY_IS_TRUE(details.Image.starts_with("sha256:"));
            VERIFY_ARE_EQUAL(details.Config.Image, "debian:latest");
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

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Test that Config fields are populated in inspect output.
        {
            const std::string envVar = "WSLC_TEST_VAR=hello";
            const std::string workDir = "/tmp";

            WSLCContainerLauncher launcher("debian:latest", "test-container-inspect-config", {"99999"}, {envVar});
            launcher.SetEntrypoint({"sleep"});
            launcher.SetWorkingDirectory(std::string{workDir});
            launcher.SetUser("nobody");

            auto container = launcher.Launch(*m_defaultSession);
            auto details = container.Inspect();

            const auto& config = details.Config;

            VERIFY_IS_TRUE(config.Env.has_value());
            VERIFY_IS_TRUE(std::ranges::find(*config.Env, envVar) != config.Env->end());

            VERIFY_ARE_EQUAL(config.WorkingDir, workDir);

            VERIFY_IS_TRUE(config.Cmd.has_value());
            VERIFY_ARE_EQUAL(1u, config.Cmd->size());
            VERIFY_ARE_EQUAL(config.Cmd->at(0), std::string{"99999"});

            VERIFY_IS_TRUE(config.Entrypoint.has_value());
            VERIFY_ARE_EQUAL(1u, config.Entrypoint->size());
            VERIFY_ARE_EQUAL(config.Entrypoint->at(0), std::string{"sleep"});

            VERIFY_ARE_EQUAL(config.User, std::string{"nobody"});
        }
    }

    WSLC_TEST_METHOD(Exec)
    {
        // Create a container.
        WSLCContainerLauncher launcher("debian:latest", "test-container-exec", {"sleep", "99999"}, {}, "none");

        auto container = launcher.Launch(*m_defaultSession);

        // Simple exec case.
        {
            auto process = WSLCProcessLauncher({}, {"echo", "OK"}).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "OK\n"}});
        }

        // Validate that Exec rejects invalid flags and a null output pointer.
        {
            WSLCProcessOptions options{};
            wil::com_ptr<IWSLCProcess> process;

            // A null output pointer is rejected by the marshaller.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), container.Get().Exec(&options, nullptr, nullptr));

            // Invalid process flags are rejected with E_INVALIDARG.
            options.Flags = static_cast<WSLCProcessFlags>(0x4);
            VERIFY_ARE_EQUAL(E_INVALIDARG, container.Get().Exec(&options, nullptr, &process));
        }

        // Validate that the working directory is correctly wired.
        {
            WSLCProcessLauncher launcher({}, {"pwd"});
            launcher.SetWorkingDirectory("/tmp");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "/tmp\n"}});
        }

        // Validate that the username is correctly wired.
        {
            WSLCProcessLauncher launcher({}, {"whoami"});
            launcher.SetUser("nobody");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "nobody\n"}});
        }

        // Validate that the group is correctly wired.
        {
            WSLCProcessLauncher launcher({}, {"groups"});
            launcher.SetUser("nobody:www-data");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "www-data\n"}});
        }

        // Validate that stdin is correctly wired.
        {
            auto process = WSLCProcessLauncher({}, {"/bin/cat"}, {}, WSLCProcessFlagsStdin).Launch(container.Get());

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
            auto process = WSLCProcessLauncher({}, {"/bin/cat"}, {}, WSLCProcessFlagsStdin).Launch(container.Get());

            process.GetStdHandle(0); // Close stdin.
            ValidateProcessOutput(process, {{1, ""}, {2, ""}});
        }

        // Validate that exit codes are correctly wired.
        {
            auto process = WSLCProcessLauncher({}, {"/bin/sh", "-c", "exit 12"}, {}).Launch(container.Get());
            ValidateProcessOutput(process, {}, 12);
        }

        // Validate that environment is correctly wired.
        {
            auto process = WSLCProcessLauncher({}, {"/bin/sh", "-c", "echo $testenv"}, {{"testenv=testvalue"}}).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "testvalue\n"}});
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLCProcessLauncher launcher({}, {"echo", "foo", "", "bar"});

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // Expect two spaces for the empty argument.
        }

        // Validate that launching a non-existing command returns the correct error.

        {
            WSLCProcessLauncher launcher({}, {"/not-found"});

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(
                process,
                {{1,
                  "OCI runtime exec failed: exec failed: unable to start container process: exec: \"/not-found\": stat "
                  "/not-found: no such file or directory: unknown\r\n"}},
                126);
        }

        // Validate that setting invalid current directory returns the correct error.
        {
            WSLCProcessLauncher launcher({}, {"/bin/cat"});
            launcher.SetWorkingDirectory("/notfound");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(
                process,
                {{1,
                  "OCI runtime exec failed: exec failed: unable to start container process: chdir to cwd (\"/notfound\") set in "
                  "config.json failed: no such file or directory: unknown\r\n"}},
                126);
        }

        // Validate that invalid usernames are correctly handled.
        {
            WSLCProcessLauncher launcher({}, {"/bin/cat"});
            launcher.SetUser("does-not-exist");

            auto process = launcher.Launch(container.Get());
            ValidateProcessOutput(process, {{1, "unable to find user does-not-exist: no matching entries in passwd file\r\n"}}, 126);
        }

        // Validate that an exec'd command returns when the container is stopped.
        {
            auto process = WSLCProcessLauncher({}, {"/bin/cat"}, {}, WSLCProcessFlagsStdin).Launch(container.Get());

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 128 + WSLCSignalSIGKILL);
        }

        // Validate that processes can't be launched in stopped containers.
        {
            auto id = container.Id();
            auto [result, _] = WSLCProcessLauncher({}, {"/bin/cat"}).LaunchNoThrow(container.Get());

            VERIFY_ARE_EQUAL(result, WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));
        }

        // Validate that invalid tty sizes are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "invalid-tty-size-exec", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);

            WSLCProcessLauncher execLauncher({}, {"/bin/sh", "-c", "stty size"}, {}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);
            execLauncher.SetTtySize(0, 0);

            auto [result, process] = execLauncher.LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ExecContainerDelete)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-exec-dtor", {"sleep", "99999"}, {}, "none");

        auto container = launcher.Launch(*m_defaultSession);

        auto process = WSLCProcessLauncher({}, {"sleep", "99999"}).Launch(container.Get());
        auto exitEvent = process.GetExitEvent();

        // Destroy the container (Stop + Delete + release COM reference).
        VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
        VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        container.Reset();

        // The exec process exit event must be signaled within a reasonable timeout.
        VERIFY_IS_TRUE(exitEvent.wait(30 * 1000));
        VERIFY_ARE_EQUAL(process.GetExitCode(), 128 + WSLCSignalSIGKILL);
    }

    // Stopping a container releases every in-flight exec from inside the Docker 'die' event callback. Several execs are
    // required: releasing them unregisters their event callbacks while the tracker is dispatching that same event.
    WSLC_TEST_METHOD(ExecContainerStopManyExecs)
    {
        constexpr unsigned int c_execCount = 8;

        WSLCContainerLauncher launcher("debian:latest", "test-exec-stop-many", {"sleep", "99999"}, {}, "none");
        auto container = launcher.Launch(*m_defaultSession);

        std::vector<ClientRunningWSLCProcess> processes;
        std::vector<wil::unique_event> exitEvents;
        processes.reserve(c_execCount);
        exitEvents.reserve(c_execCount);

        for (unsigned int i = 0; i < c_execCount; ++i)
        {
            processes.emplace_back(WSLCProcessLauncher({}, {"sleep", "99999"}).Launch(container.Get()));
            exitEvents.emplace_back(processes.back().GetExitEvent());
        }

        VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

        // No exec may be skipped when the container releases them.
        for (unsigned int i = 0; i < c_execCount; ++i)
        {
            VERIFY_IS_TRUE(exitEvents[i].wait(30 * 1000));
            VERIFY_ARE_EQUAL(processes[i].GetExitCode(), 128 + WSLCSignalSIGKILL);
        }

        // Lifecycle events must still be delivered once the stop has been processed.
        VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr));
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }

    // Exec() registers its event callback while holding the container lock, concurrently with the Docker event thread
    // delivering exec_die for other execs on the same container.
    WSLC_TEST_METHOD(ExecContainerEventStress)
    {
        constexpr unsigned int c_threadCount = 4;
        constexpr unsigned int c_iterationsPerThread = 25;

        WSLCContainerLauncher launcher("debian:latest", "test-exec-event-stress", {"sleep", "99999"}, {}, "none");
        auto container = launcher.Launch(*m_defaultSession);

        std::atomic<unsigned int> failures = 0;
        std::vector<std::thread> threads;
        threads.reserve(c_threadCount);

        for (unsigned int t = 0; t < c_threadCount; ++t)
        {
            threads.emplace_back([&]() {
                for (unsigned int i = 0; i < c_iterationsPerThread; ++i)
                {
                    // N.B. Each process is released without waiting, so its callback is unregistered while exec_die
                    // events are still being dispatched.
                    auto [result, process] = WSLCProcessLauncher({}, {"/bin/true"}).LaunchNoThrow(container.Get());
                    if (FAILED(result))
                    {
                        LogError("Exec unexpected HR: 0x%08x", result);
                        ++failures;
                        return;
                    }
                }
            });
        }

        for (auto& thread : threads)
        {
            thread.join();
        }

        VERIFY_ARE_EQUAL(failures.load(), 0u);

        // The event stream must still be live after the exec_die storm.
        VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
    }
};
