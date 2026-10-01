/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerLogsTests.cpp

Abstract:

    This file contains test cases for the WSLC container logs API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerLogsTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerLogsTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(ContainerLogs)
    {
        auto expectLogs = [](auto& container,
                             const std::string& expectedStdout,
                             const std::optional<std::string>& expectedStderr,
                             WSLCLogsFlags Flags = WSLCLogsFlagsNone,
                             ULONGLONG Tail = 0,
                             ULONGLONG Since = 0,
                             ULONGLONG Until = 0) {
            COMOutputHandle stdoutHandle;
            COMOutputHandle stderrHandle;
            VERIFY_SUCCEEDED(container.Logs(Flags, &stdoutHandle, &stderrHandle, Since, Until, Tail));

            ValidateHandleOutput(stdoutHandle.Get(), expectedStdout);

            if (expectedStderr.has_value())
            {
                ValidateHandleOutput(stderrHandle.Get(), expectedStderr.value());
            }
        };

        // Test a simple scenario.
        {
            // Create a container with a simple command.
            WSLCContainerLauncher launcher(
                "debian:latest", "logs-test-1", {"/bin/bash", "-c", "echo stdout && (echo stderr >& 2)"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "stdout\n"}, {2, "stderr\n"}});

            expectLogs(container.Get(), "stdout\n", "stderr\n");

            // validate that logs can be queried multiple times.
            expectLogs(container.Get(), "stdout\n", "stderr\n");
        }

        // Validate that tail works.
        {
            // Create a container with a simple command.
            WSLCContainerLauncher launcher(
                "debian:latest", "logs-test-2", {"/bin/bash", "-c", "echo -en 'line1\\nline2\\nline3\\nline4'"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "line1\nline2\nline3\nline4"}});

            expectLogs(container.Get(), "line1\nline2\nline3\nline4", "");
            expectLogs(container.Get(), "line4", "", WSLCLogsFlagsNone, 1);
            expectLogs(container.Get(), "line3\nline4", "", WSLCLogsFlagsNone, 2);
            expectLogs(container.Get(), "line1\nline2\nline3\nline4", "", WSLCLogsFlagsNone, 4);
        }

        // Validate that timestamps are correctly returned.
        {
            WSLCContainerLauncher launcher("debian:latest", "logs-test-3", {"/bin/bash", "-c", "echo -n OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OK"}});

            COMOutputHandle stdoutHandle{};
            COMOutputHandle stderrHandle{};
            VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsTimestamps, &stdoutHandle, &stderrHandle, 0, 0, 0));

            auto output = ReadToString(stdoutHandle.Get());
            VerifyPatternMatch(output, "20*-*-* OK"); // Timestamp is in ISO 8601 format
        }

        // Validate that 'since' and 'until' work as expected.
        {
            WSLCContainerLauncher launcher("debian:latest", "logs-test-4", {"/bin/bash", "-c", "echo -n OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OK"}});

            // Testing would with more granularity would be difficult, but these flags are just forwarded to docker,
            // so validate that they're wired correctly.

            auto now = time(nullptr);
            expectLogs(container.Get(), "OK", "", WSLCLogsFlagsNone, 0, now - 3600);
            expectLogs(container.Get(), "", "", WSLCLogsFlagsNone, 0, now + 3600);

            expectLogs(container.Get(), "", "", WSLCLogsFlagsNone, 0, 0, now - 3600);
            expectLogs(container.Get(), "OK", "", WSLCLogsFlagsNone, 0, 0, now + 3600);
        }

        // Validate that logs work for TTY processes
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "logs-test-5", {"/bin/bash", "-c", "stat -f /dev/stdin | grep -io 'Type:.*$'"}, {}, {}, WSLCProcessFlagsStdin | WSLCProcessFlagsTty);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            ValidateHandleOutput(initProcess.GetStdHandle(WSLCFDTty).Get(), "Type: devpts\r\n");
            VERIFY_ARE_EQUAL(initProcess.Wait(), 0);

            expectLogs(container.Get(), "Type: devpts\r\n", {});

            // Validate that logs can queried multiple times.
            expectLogs(container.Get(), "Type: devpts\r\n", {});
        }

        // Validate that the 'follow' flag works as expected.
        {
            WSLCContainerLauncher launcher("debian:latest", "logs-test-6", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            // Without 'follow', logs return immediately.
            expectLogs(container.Get(), "", "");

            // Create a 'follow' logs call.
            COMOutputHandle stdoutHandle{};
            COMOutputHandle stderrHandle{};
            VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsFollow, &stdoutHandle, &stderrHandle, 0, 0, 0));

            PartialHandleRead reader(stdoutHandle.Get());

            auto containerStdin = initProcess.GetStdHandle(0);
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.Get(), "line1\n", 6, nullptr, nullptr));

            reader.Expect("line1\n");
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.Get(), "line2\n", 6, nullptr, nullptr));
            reader.Expect("line1\nline2\n");

            containerStdin.Reset();
            reader.ExpectClosed();

            expectLogs(container.Get(), "line1\nline2\n", "");
            expectLogs(container.Get(), "line1\nline2\n", "", WSLCLogsFlagsFollow);
        }

        // Validate that invalid logs flags are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "logs-test-invalid-flags", {"/bin/bash", "-c", "echo OK"});
            auto container = launcher.Create(*m_defaultSession);

            COMOutputHandle stdoutHandle{};
            COMOutputHandle stderrHandle{};
            VERIFY_ARE_EQUAL(container.Get().Logs(static_cast<WSLCLogsFlags>(0x8), &stdoutHandle, &stderrHandle, 0, 0, 0), E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerLogsManyConcurrentFollowers)
    {
        constexpr size_t followerCount = 100;
        static_assert(followerCount > MAXIMUM_WAIT_OBJECTS);

        WSLCContainerLauncher launcher("debian:latest", "logs-test-many-followers", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
        auto container = launcher.Launch(*m_defaultSession);
        auto initProcess = container.GetInitProcess();

        auto containerStdin = initProcess.GetStdHandle(0);
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.Get(), "OK\n", 3, nullptr, nullptr));

        std::atomic<size_t> readersReady{0};
        std::atomic<size_t> readersSucceeded{0};
        std::vector<std::thread> threads;
        threads.reserve(followerCount);

        for (size_t i = 0; i < followerCount; ++i)
        {
            threads.emplace_back([&]() {
                try
                {
                    COMOutputHandle stdoutHandle{};
                    COMOutputHandle stderrHandle{};
                    VERIFY_SUCCEEDED(container.Get().Logs(WSLCLogsFlagsFollow, &stdoutHandle, &stderrHandle, 0, 0, 0));

                    PartialHandleRead reader(stdoutHandle.Get());
                    reader.Expect("OK\n");
                    readersReady.fetch_add(1);
                    reader.ExpectClosed();
                    readersSucceeded.fetch_add(1);
                }
                CATCH_LOG();
            });
        }

        // Wait until every follower has observed the marker before killing the container.
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(E_ABORT, readersReady.load() < followerCount); }, std::chrono::milliseconds(100), std::chrono::seconds(120));

        // Kill the container so all follow handles are closed.
        VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

        for (auto& t : threads)
        {
            t.join();
        }

        VERIFY_ARE_EQUAL(readersSucceeded.load(), followerCount);
    }

    WSLC_TEST_METHOD(ContainerLabels)
    {
        // Docker labels do not have a size limit, so test with a very large label value to validate that the API can handle it.
        std::map<std::string, std::string> labels = {{"key1", "value1"}, {"key2", std::string(10000, 'a')}};
        const std::string c_image = "debian:latest";

        // Contains-style rather than exact-equality so the test stays green if the base image ever ships with its own labels.
        auto verifyUserLabelsPresent = [&](const std::map<std::string, std::string>& observed) {
            for (const auto& [key, value] : labels)
            {
                auto it = observed.find(key);
                VERIFY_IS_TRUE(it != observed.end());
                if (it != observed.end())
                {
                    VERIFY_ARE_EQUAL(value, it->second);
                }
            }
        };

        // Test valid labels
        {
            WSLCContainerLauncher launcher(c_image, "test-labels", {"echo", "OK"});

            for (const auto& [key, value] : labels)
            {
                launcher.AddLabel(key, value);
            }

            auto container = launcher.Launch(*m_defaultSession);
            const auto containerLabels = container.Labels();
            verifyUserLabelsPresent(containerLabels);
            VERIFY_IS_TRUE(containerLabels.find("com.microsoft.wsl.container.metadata") == containerLabels.end());

            const auto inspect = container.Inspect();
            VERIFY_ARE_EQUAL(c_image, inspect.Config.Image);
            VERIFY_IS_TRUE(inspect.Image.starts_with("sha256:"));

            // Keep the container alive after the handle is dropped so we can validate labels are persisted across sessions.
            container.SetDeleteOnClose(false);
        }

        {
            // Restarting the test session will force the container to be reloaded from storage.
            ResetTestSession();

            // Validate that labels are correctly loaded.
            auto container = OpenContainer(m_defaultSession.get(), "test-labels");
            const auto containerLabels = container.Labels();
            verifyUserLabelsPresent(containerLabels);

            const std::string c_metadataLabel = "com.microsoft.wsl.container.metadata";
            VERIFY_IS_TRUE(containerLabels.find(c_metadataLabel) == containerLabels.end());
            const auto inspect = container.Inspect();
            verifyUserLabelsPresent(inspect.Config.Labels);
            verifyUserLabelsPresent(inspect.Labels);
            VERIFY_ARE_EQUAL(inspect.Config.Labels, inspect.Labels);
            VERIFY_IS_TRUE(inspect.Config.Labels.find(c_metadataLabel) == inspect.Config.Labels.end());
            VERIFY_ARE_EQUAL(c_image, inspect.Config.Image);
            VERIFY_IS_TRUE(inspect.Image.starts_with("sha256:"));
        }

        // Test nullptr key
        {
            WSLCLabel label{.Key = nullptr, .Value = "value"};

            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-nullptr-key";
            options.Labels = &label;
            options.LabelsCount = 1;

            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test nullptr value
        {
            WSLCLabel label{.Key = "key", .Value = nullptr};

            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-nullptr-value";
            options.Labels = &label;
            options.LabelsCount = 1;

            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test duplicate keys
        {
            std::vector<WSLCLabel> labels;
            labels.push_back({.Key = "key", .Value = "value"});
            labels.push_back({.Key = "key", .Value = "value2"});

            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "test-labels-duplicate-keys";
            options.Labels = labels.data();
            options.LabelsCount = static_cast<ULONG>(labels.size());

            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
            VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        }

        // Test wslc metadata key conflict
        {
            WSLCContainerLauncher launcher("debian:latest");
            launcher.AddLabel("com.microsoft.wsl.container.metadata", "value");

            auto [hr, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }
    }

    // Regression: containers must inherit their base image's LABEL entries (Docker parity), with user --label
    // winning on key conflict. The dockerd daemon does the merge; wslc reads it back from InspectContainer.
    WSLC_TEST_METHOD(ContainerLabelsInheritedFromImage)
    {
        const std::string c_imageTag = "wslc-test-labels-inherited:latest";
        const std::string c_imageLabelKey = "com.microsoft.wsl.test.image-label";
        const std::string c_imageLabelValue = "from-image";
        const std::string c_sharedLabelKey = "com.microsoft.wsl.test.shared";
        const std::string c_sharedImageValue = "image-wins-if-no-override";
        const std::string c_sharedUserValue = "user-wins";
        const std::string c_userOnlyLabelKey = "com.microsoft.wsl.test.user-only";
        const std::string c_userOnlyLabelValue = "from-user";
        const std::string c_metadataLabel = "com.microsoft.wsl.container.metadata";
        const std::string c_userOverrideContainerName = "test-labels-inherited-user-override";

        auto contextDir = std::filesystem::current_path() / "build-context-labels-inherited";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow(c_imageTag.c_str(), WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "LABEL " << c_imageLabelKey << "=" << c_imageLabelValue << "\n";
            dockerfile << "LABEL " << c_sharedLabelKey << "=" << c_sharedImageValue << "\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, c_imageTag.c_str()));
        ExpectImagePresent(*m_defaultSession, c_imageTag.c_str());

        // Image-only label survives on the container (bug repro).
        {
            WSLCContainerLauncher launcher(c_imageTag.c_str(), "test-labels-inherited-image-only", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);

            const auto containerLabels = container.Labels();
            const auto inspect = container.Inspect();

            VERIFY_IS_TRUE(containerLabels.contains(c_imageLabelKey));
            VERIFY_ARE_EQUAL(c_imageLabelValue, containerLabels.at(c_imageLabelKey));
            VERIFY_IS_TRUE(inspect.Config.Labels.contains(c_imageLabelKey));
            VERIFY_ARE_EQUAL(c_imageLabelValue, inspect.Config.Labels.at(c_imageLabelKey));

            VERIFY_IS_TRUE(containerLabels.find(c_metadataLabel) == containerLabels.end());
        }

        // Persist across a session reset so the second block exercises the Open() codepath, which reads labels
        // from the /containers/json list-API — a different deserialization than InspectContainer.Config.Labels.
        {
            WSLCContainerLauncher launcher(c_imageTag.c_str(), c_userOverrideContainerName.c_str(), {"echo", "OK"});
            launcher.AddLabel(c_sharedLabelKey, c_sharedUserValue);
            launcher.AddLabel(c_userOnlyLabelKey, c_userOnlyLabelValue);
            auto container = launcher.Launch(*m_defaultSession);

            const auto containerLabels = container.Labels();

            VERIFY_IS_TRUE(containerLabels.contains(c_imageLabelKey));
            VERIFY_ARE_EQUAL(c_imageLabelValue, containerLabels.at(c_imageLabelKey));

            VERIFY_IS_TRUE(containerLabels.contains(c_sharedLabelKey));
            VERIFY_ARE_EQUAL(c_sharedUserValue, containerLabels.at(c_sharedLabelKey));

            VERIFY_IS_TRUE(containerLabels.contains(c_userOnlyLabelKey));
            VERIFY_ARE_EQUAL(c_userOnlyLabelValue, containerLabels.at(c_userOnlyLabelKey));

            container.SetDeleteOnClose(false);
        }

        {
            ResetTestSession();

            auto reopened = OpenContainer(m_defaultSession.get(), c_userOverrideContainerName.c_str());
            const auto reopenedLabels = reopened.Labels();

            VERIFY_IS_TRUE(reopenedLabels.contains(c_imageLabelKey));
            VERIFY_ARE_EQUAL(c_imageLabelValue, reopenedLabels.at(c_imageLabelKey));
            VERIFY_IS_TRUE(reopenedLabels.contains(c_sharedLabelKey));
            VERIFY_ARE_EQUAL(c_sharedUserValue, reopenedLabels.at(c_sharedLabelKey));
            VERIFY_IS_TRUE(reopenedLabels.contains(c_userOnlyLabelKey));
            VERIFY_ARE_EQUAL(c_userOnlyLabelValue, reopenedLabels.at(c_userOnlyLabelKey));

            VERIFY_IS_TRUE(reopenedLabels.find(c_metadataLabel) == reopenedLabels.end());

            const auto reopenedInspect = reopened.Inspect();
            VERIFY_IS_TRUE(reopenedInspect.Config.Labels.contains(c_imageLabelKey));
            VERIFY_ARE_EQUAL(c_imageLabelValue, reopenedInspect.Config.Labels.at(c_imageLabelKey));
        }
    }

    WSLC_TEST_METHOD(ContainerResourceLimits)
    {
        // Validate per-container memory limit is applied (cgroup v2: /sys/fs/cgroup/memory.max).
        {
            constexpr std::int64_t memoryBytes = 64 * 1024 * 1024; // 64 MiB
            WSLCContainerLauncher launcher("debian:latest", "test-container-memory-limit", {"cat", "/sys/fs/cgroup/memory.max"});
            launcher.SetMemoryLimit(memoryBytes);

            ValidateContainerOutput(launcher, {{1, std::format("{}\n", memoryBytes)}});
        }

        // Validate per-container CPU quota is applied (cgroup v2: /sys/fs/cgroup/cpu.max).
        // NanoCpus = 1.5 * 1e9 -> quota=150000 period=100000.
        {
            constexpr std::int64_t nanoCpus = 1'500'000'000ll;
            WSLCContainerLauncher launcher("debian:latest", "test-container-cpu-limit", {"cat", "/sys/fs/cgroup/cpu.max"});
            launcher.SetNanoCpus(nanoCpus);

            ValidateContainerOutput(launcher, {{1, "150000 100000\n"}});
        }

        // Validate ulimit (nofile) is applied to the init process.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-ulimit", {"sh", "-c", "ulimit -Sn; ulimit -Hn"});
            launcher.AddUlimit("nofile", 1234, 5678);

            ValidateContainerOutput(launcher, {{1, "1234\n5678\n"}});
        }

        // Validate that the configured limits are reported back via container.Inspect().
        {
            constexpr std::int64_t memoryBytes = 64 * 1024 * 1024;
            constexpr std::int64_t nanoCpus = 500'000'000ll;

            WSLCContainerLauncher launcher("debian:latest", "test-container-limits-inspect", {"true"});
            launcher.SetMemoryLimit(memoryBytes);
            launcher.SetNanoCpus(nanoCpus);
            launcher.AddUlimit("nofile", 1234, 5678);

            auto container = launcher.Create(*m_defaultSession);
            auto hostConfig = container.Inspect().HostConfig;

            VERIFY_ARE_EQUAL(memoryBytes, hostConfig.Memory);
            VERIFY_ARE_EQUAL(nanoCpus, hostConfig.NanoCpus);
            VERIFY_ARE_EQUAL(static_cast<size_t>(1), hostConfig.Ulimits.size());
            VERIFY_ARE_EQUAL(std::string("nofile"), hostConfig.Ulimits[0].Name);
            VERIFY_ARE_EQUAL(1234ll, hostConfig.Ulimits[0].Soft);
            VERIFY_ARE_EQUAL(5678ll, hostConfig.Ulimits[0].Hard);
        }

        // Validate inspect defaults when no limits are configured: Memory/NanoCpus are 0 ("no limit") and Ulimits is empty.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-limits-inspect-defaults", {"true"});

            auto container = launcher.Create(*m_defaultSession);
            auto hostConfig = container.Inspect().HostConfig;

            VERIFY_ARE_EQUAL(0ll, hostConfig.Memory);
            VERIFY_ARE_EQUAL(0ll, hostConfig.NanoCpus);
            VERIFY_IS_TRUE(hostConfig.Ulimits.empty());
        }

        // Validate that multiple ulimits round-trip through inspect in the order they were configured.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-limits-inspect-multi", {"true"});
            launcher.AddUlimit("nofile", 1234, 5678);
            launcher.AddUlimit("nproc", 100, 200);

            auto container = launcher.Create(*m_defaultSession);
            auto ulimits = container.Inspect().HostConfig.Ulimits;

            VERIFY_ARE_EQUAL(static_cast<size_t>(2), ulimits.size());
            VERIFY_ARE_EQUAL(std::string("nofile"), ulimits[0].Name);
            VERIFY_ARE_EQUAL(1234ll, ulimits[0].Soft);
            VERIFY_ARE_EQUAL(5678ll, ulimits[0].Hard);
            VERIFY_ARE_EQUAL(std::string("nproc"), ulimits[1].Name);
            VERIFY_ARE_EQUAL(100ll, ulimits[1].Soft);
            VERIFY_ARE_EQUAL(200ll, ulimits[1].Hard);
        }

        // Validate that a Ulimit entry with a null Name is rejected.
        {
            WSLCUlimit ulimit{.Name = nullptr, .Soft = 1, .Hard = 1};

            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "test-ulimit-null-name";
            options.Ulimits = &ulimit;
            options.UlimitsCount = 1;

            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }
    }

    WSLC_TEST_METHOD(ContainerAttach)
    {
        // Validate attach behavior in a non-tty process.
        {
            WSLCContainerLauncher launcher("debian:latest", "attach-test-1", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            // Verify that attaching to a created container fails.
            COMOutputHandle stdinHandle{};
            COMOutputHandle stdoutHandle{};
            COMOutputHandle stderrHandle{};
            auto id = container->Id();
            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle), WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));

            // Start the container.
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));

            // Verify that trying to attach with null handles fails.
            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, nullptr, nullptr, nullptr), HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));

            // Get its original std handles.
            auto process = container->GetInitProcess();
            auto originalStdin = process.GetStdHandle(0);
            auto originalStdout = process.GetStdHandle(1);

            // Attach to the container with separate handles.
            stdinHandle.Reset();
            stdoutHandle.Reset();
            stderrHandle.Reset();
            VERIFY_SUCCEEDED(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle));

            PartialHandleRead originalReader(originalStdout.Get());
            PartialHandleRead attachedReader(stdoutHandle.Get());

            // Write content on the original stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(originalStdin.Get(), "line1\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\n");
            attachedReader.Expect("line1\n");

            // Write content on the attached stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(stdinHandle.Get(), "line2\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\nline2\n");
            attachedReader.Expect("line1\nline2\n");

            // Close the original stdin.
            originalStdin.Reset();

            // Expect both readers to be closed.
            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();

            process.Wait();

            stdinHandle.Reset();
            stdoutHandle.Reset();
            stderrHandle.Reset();

            // Validate that attaching to an exited container fails.
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateExited);
            stdinHandle.Reset();
            stdoutHandle.Reset();
            stderrHandle.Reset();
            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle), WSLC_E_CONTAINER_NOT_RUNNING);
            ValidateCOMErrorMessage(std::format(L"Container '{}' is not running.", id));

            // Validate that attaching to a deleted container fails.
            VERIFY_SUCCEEDED(container->Get().Delete(WSLCDeleteFlagsNone));
            stdinHandle.Reset();
            stdoutHandle.Reset();
            stderrHandle.Reset();

            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle), RPC_E_DISCONNECTED);

            container->SetDeleteOnClose(false);
        }

        // Validate that closing an attached stdin terminates the container.
        {
            WSLCContainerLauncher launcher("debian:latest", "attach-test-2", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession);

            auto process = container.GetInitProcess();
            auto originalStdin = process.GetStdHandle(0);
            auto originalStdout = process.GetStdHandle(1);

            COMOutputHandle attachedStdin;
            COMOutputHandle attachedStdout;
            COMOutputHandle attachedStderr;
            VERIFY_SUCCEEDED(container.Get().Attach(nullptr, &attachedStdin, &attachedStdout, &attachedStderr));

            PartialHandleRead originalReader(originalStdout.Get());
            PartialHandleRead attachedReader(attachedStdout.Get());

            attachedStdin.Reset();

            // Expect both readers to be closed.
            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();
        }

        // Validate behavior for tty containers
        {
            WSLCContainerLauncher launcher("debian:latest", "attach-test-3", {"/bin/bash"}, {}, {}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);

            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();
            auto originalTty = process.GetStdHandle(WSLCFDTty);

            COMOutputHandle attachedTty{};
            COMOutputHandle dummyHandle1{};
            COMOutputHandle dummyHandle2{};
            VERIFY_SUCCEEDED(container.Get().Attach(nullptr, &attachedTty, &dummyHandle1, &dummyHandle2));

            PartialHandleRead originalReader(originalTty.Get());
            PartialHandleRead attachedReader(attachedTty.Get());

            // Read the prompt from the original tty (hardcoded bytes since behavior is constant).
            auto prompt = originalReader.ReadBytes(13);
            VerifyPatternMatch(prompt, "*root@*");

            // Resize the tty to force the prompt to redraw.
            process.Get().ResizeTty(61, 81);

            auto attachedPrompt = attachedReader.ReadBytes(13);
            VerifyPatternMatch(attachedPrompt, "*root@*");

            // Stop pending reads before closing the handles borrowed by the readers.
            originalReader.Stop();
            attachedReader.Stop();

            originalTty.Reset();
            attachedTty.Reset();
        }

        // Validate that containers can be started in detached mode and attached to later.
        {
            WSLCContainerLauncher launcher("debian:latest", "attach-test-4", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto container = launcher.Launch(*m_defaultSession, WSLCContainerStartFlagsNone);

            auto initProcess = container.GetInitProcess();
            WSLCHandle dummy{};
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLCFDStdin, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLCFDStdout, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            VERIFY_ARE_EQUAL(initProcess.Get().GetStdHandle(WSLCFDStderr, &dummy), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));

            // Verify that the container can be attached to.
            COMOutputHandle attachedStdin{};
            COMOutputHandle attachedStdout{};
            COMOutputHandle attachedStderr{};
            VERIFY_SUCCEEDED(container.Get().Attach(nullptr, &attachedStdin, &attachedStdout, &attachedStderr));

            PartialHandleRead attachedReader(attachedStdout.Get());

            // Write content on the attached stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(attachedStdin.Get(), "OK\n", 3, nullptr, nullptr));
            attachedStdin.Reset();

            attachedReader.Expect("OK\n");
            attachedReader.ExpectClosed();
            VERIFY_ARE_EQUAL(initProcess.Wait(), 0);
        }
    }
};
