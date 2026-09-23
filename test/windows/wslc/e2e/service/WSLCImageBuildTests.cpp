/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCImageBuildTests.cpp

Abstract:

    This file contains test cases for the WSLC image build API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCImageBuildTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCImageBuildTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    class CapturingProgressCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
    {
    public:
        CapturingProgressCallback(std::string& output) : m_output(output)
        {
        }

        HRESULT OnProgress(LPCSTR status, LPCSTR, ULONGLONG, ULONGLONG) override
        {
            m_output.append(status);
            return S_OK;
        }

    private:
        std::string& m_output;
    };

    WSLC_TEST_METHOD(BuildImage)
    {
        auto contextDir = std::filesystem::current_path() / "build-context";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "CMD [\"echo\", \"Hello from a WSL container!\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build:latest");

        WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-build-test-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from a WSL container!") != std::string::npos);
    }

    // This test validates both that we can build an image with an empty CMD, and that we can run such an image.
    WSLC_TEST_METHOD(BuildImageEntrypoint)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-entrypoint";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-entrypoint:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "CMD []\n";
            dockerfile << "ENTRYPOINT [\"/bin/echo\", \"Entrypoint\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-entrypoint:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-entrypoint:latest");

        // Validate that the entrypoint is started by default.
        {
            WSLCContainerLauncher launcher("wslc-test-entrypoint:latest", "wslc-entrypoint-test-1");
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "Entrypoint\n"}});
        }

        // Validate that arguments are passed to the entrypoint, and don't override it.
        {
            WSLCContainerLauncher launcher("wslc-test-entrypoint:latest", "wslc-entrypoint-test-2", {"extra-arg"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "Entrypoint extra-arg\n"}});
        }

        // Validate that the entrypoint can be overridden.
        {
            WSLCContainerLauncher launcher("wslc-test-entrypoint:latest", "wslc-entrypoint-test-3");
            launcher.SetEntrypoint({"/bin/echo", "OverriddenEntrypoint"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OverriddenEntrypoint\n"}});
        }

        // Validate that the entrypoint can be overridden and that CMD args are passed to the entrypoint.
        {
            WSLCContainerLauncher launcher("wslc-test-entrypoint:latest", "wslc-entrypoint-test-4", {"extra-arg"});
            launcher.SetEntrypoint({"/bin/echo", "OverriddenEntrypoint"});
            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();
            ValidateProcessOutput(initProcess, {{1, "OverriddenEntrypoint extra-arg\n"}});
        }
    }

    WSLC_TEST_METHOD(BuildImageHealthCheck)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-healthcheck";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-healthcheck:latest", WSLCDeleteImageFlagsForce).first);
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        // Create an image with a healthcheck that only passes once a specific file exists.
        constexpr auto c_healthReadyFile = "/tmp/wslc-health-ready";

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "HEALTHCHECK --interval=1s --timeout=100ms --start-period=300s --retries=1000 CMD test -f "
                       << c_healthReadyFile << "\n";
            dockerfile << "CMD [\"sleep\", \"99999\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-healthcheck:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-healthcheck:latest");

        auto waitForHealthStatus = [](auto& container, const std::string& expectedStatus, std::chrono::seconds timeout) {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    const auto inspect = container.Inspect();
                    THROW_HR_IF_MSG(E_FAIL, !inspect.State.Health.has_value(), "container does not report a health status yet");
                    THROW_HR_IF_MSG(
                        E_FAIL,
                        inspect.State.Health->Status != expectedStatus,
                        "health status is '%hs', expected '%hs'",
                        inspect.State.Health->Status.c_str(),
                        expectedStatus.c_str());
                },
                std::chrono::milliseconds{100},
                timeout);
        };

        // Validate that the image's default health check is inherited by a started container, and that its runtime
        // status stays "starting" until the health command passes, then deterministically becomes "healthy".
        {
            WSLCContainerLauncher launcher("wslc-test-healthcheck:latest", "wslc-healthcheck-test-default");
            auto container = launcher.Launch(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"CMD-SHELL", std::string("test -f ") + c_healthReadyFile};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.Interval.value_or(0));
            VERIFY_ARE_EQUAL(100'000'000LL, health.Timeout.value_or(0));
            VERIFY_ARE_EQUAL(300'000'000'000LL, health.StartPeriod.value_or(0));

            // The health command fails while the file is absent, so the container stays "starting".
            waitForHealthStatus(container, "starting", 60s);

            auto touchProcess = WSLCProcessLauncher({}, {"/usr/bin/touch", c_healthReadyFile}).Launch(container.Get());
            ValidateProcessOutput(touchProcess, {}, 0);

            waitForHealthStatus(container, "healthy", 60s);
        }

        // Validate that the image's default health check can be overridden, and that a failing (exit 1) check drives
        // the runtime status to "unhealthy".
        {
            WSLCContainerLauncher launcher("wslc-test-healthcheck:latest", "wslc-healthcheck-test-override");
            launcher.SetHealthCmd("exit 1");
            launcher.SetHealthInterval(1'000'000'000LL);    // 1s
            launcher.SetHealthStartPeriod(1'000'000'000LL); // 1s
            launcher.SetHealthRetries(1);
            auto container = launcher.Launch(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"CMD-SHELL", "exit 1"};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.Interval.value_or(0));
            // The override must set an explicit start period: otherwise the engine merges the image's healthcheck
            // fields for any zero-valued field (see moby daemon merge()), inheriting the image's 300s start period,
            // during which failing checks keep the container "starting" instead of transitioning to "unhealthy".
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.StartPeriod.value_or(0));
            VERIFY_ARE_EQUAL(1, health.Retries.value_or(0));

            // Validate that the container transitions to "unhealthy" after the health command fails.
            waitForHealthStatus(container, "unhealthy", 60s);
        }

        // Validate that WSLCContainerFlagsNoHealthCheck disables the image's default health check.
        {
            WSLCContainerLauncher launcher("wslc-test-healthcheck:latest", "wslc-healthcheck-test-disabled");
            launcher.SetNoHealthcheck();
            auto container = launcher.Launch(*m_defaultSession);

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"NONE"};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());

            // A disabled health check is not monitored, so the container never reports a runtime health status.
            VERIFY_IS_FALSE(inspect.State.Health.has_value());
        }

        // Validate that combining WSLCContainerFlagsNoHealthCheck with an explicit health check command is rejected.
        {
            WSLCContainerLauncher launcher("wslc-test-healthcheck:latest", "wslc-healthcheck-test-conflict");
            launcher.SetNoHealthcheck();
            launcher.SetHealthCmd("exit 0");

            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(result, E_INVALIDARG);
            VERIFY_IS_FALSE(container.has_value());
        }
    }

    WSLC_TEST_METHOD(BuildImageWithContext)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-file";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-context:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY message.txt /message.txt\n";
            dockerfile << "CMD [\"cat\", \"/message.txt\"]\n";
        }

        {
            std::ofstream message(contextDir / "message.txt");
            message << "Hello from a WSL container context file!\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build-context:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-context:latest");

        WSLCContainerLauncher launcher("wslc-test-build-context:latest", "wslc-build-context-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from a WSL container context file!") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageManyFiles)
    {
        static constexpr int fileCount = 1024;

        auto contextDir = std::filesystem::current_path() / "build-context-many";
        std::filesystem::create_directories(contextDir / "files");
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-many:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        // Generate the context files.
        for (int i = 0; i < fileCount; i++)
        {
            auto name = std::format("file{:04d}.txt", i);
            auto content = std::format("content-{:04d}\n", i);
            std::ofstream file(contextDir / "files" / name);
            file << content;
        }

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY files/ /files/\n";
            // Verify every file is present and contains the expected content.
            // Only mismatches are printed; on success just the sentinel.
            dockerfile << "CMD [\"sh\", \"-c\", "
                       << "\"cd /files && failed=0 && "
                       << "for i in $(seq 0 " << (fileCount - 1) << "); do "
                       << "f=$(printf 'file%04d.txt' $i); "
                       << "e=$(printf 'content-%04d' $i); "
                       << "if [ ! -f $f ]; then echo MISSING:$f; failed=1; "
                       << "elif ! grep -q $e $f; then echo BAD:$f; failed=1; fi; "
                       << "done && "
                       << "[ $failed -eq 0 ] && echo all_ok_" << fileCount << "\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build-many:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-many:latest");

        WSLCContainerLauncher launcher("wslc-test-build-many:latest", "wslc-build-many-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        auto sentinel = std::format("all_ok_{}", fileCount);
        VERIFY_IS_TRUE(result.Output[1].find(sentinel) != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageLargeFile)
    {
        RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rmi", "-f", "wslc-test-build-large:latest"});
        ExpectCommandResult(m_defaultSession.get(), {"/usr/bin/docker", "builder", "prune", "-f"}, 0);

        auto contextDir = std::filesystem::current_path() / "build-context-large";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-large:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        static constexpr int fileSizeMb = 1024;

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY large.bin /large.bin\n";
            dockerfile << std::format(
                "CMD [\"sh\", \"-c\", \"test $(stat -c %s /large.bin) -eq {} && echo size_ok\"]\n",
                static_cast<long long>(fileSizeMb) * 1024 * 1024);
        }

        {
            auto largePath = contextDir / "large.bin";
            wil::unique_hfile largeFile{CreateFileW(largePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == largeFile.get());

            std::vector<char> buffer(1024 * 1024, '\0');
            for (int i = 0; i < fileSizeMb; i++)
            {
                DWORD written = 0;
                if (!WriteFile(largeFile.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) ||
                    written != static_cast<DWORD>(buffer.size()))
                {
                    LogError("WriteFile failed at chunk %d/%d: 0x%08x", i, fileSizeMb, GetLastError());
                    VERIFY_FAIL();
                }
            }
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build-large:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-large:latest");

        WSLCContainerLauncher launcher("wslc-test-build-large:latest", "wslc-build-large-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("size_ok") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageMultiStage)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-multistage";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-multistage:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            // Two independent stages that can build in parallel, each producing
            // part of the final output.  The last stage combines them.
            dockerfile << "FROM debian:latest AS greeting\n";
            dockerfile << "RUN echo -n 'WSL containers' | tee /part.txt\n";
            dockerfile << "\n";
            dockerfile << "FROM debian:latest AS description\n";
            dockerfile << "RUN echo -n 'support multi-stage builds' | tee /part.txt\n";
            dockerfile << "\n";
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY --from=greeting /part.txt /greeting.txt\n";
            dockerfile << "COPY --from=description /part.txt /description.txt\n";
            dockerfile << "CMD [\"sh\", \"-c\", "
                       << "\"echo \\\"$(cat /greeting.txt) $(cat /description.txt)\\\"\"]\n";
        }

        std::string output;
        auto callback = Microsoft::WRL::Make<CapturingProgressCallback>(output);
        LPCSTR tag = "wslc-test-build-multistage:latest";
        WSLCBuildImageOptions options{.Tags = {&tag, 1}, .Flags = WSLCBuildImageFlagsNoCache};
        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, &options, callback.Get()));
        VERIFY_IS_TRUE(output.find("[greeting] WSL containers") != std::string::npos);
        VERIFY_IS_TRUE(output.find("[description] support multi-stage builds") != std::string::npos);
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-multistage:latest");

        WSLCContainerLauncher launcher("wslc-test-build-multistage:latest", "wslc-build-multistage-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("WSL containers support multi-stage builds") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageDockerIgnore)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-dockerignore";
        std::filesystem::create_directories(contextDir / "temp");
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-dockerignore:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream ignore(contextDir / ".dockerignore");
            ignore << "# Ignore log files and temp directory\n";
            ignore << "*.log\n";
            ignore << "temp/\n";
        }

        {
            std::ofstream(contextDir / "keep.txt") << "kept\n";
            std::ofstream(contextDir / "debug.log") << "excluded\n";
            std::ofstream(contextDir / "temp" / "cache.dat") << "excluded\n";
        }

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY . /ctx/\n";
            dockerfile << "CMD [\"sh\", \"-c\", "
                       << "\"test -f /ctx/keep.txt "
                       << "&& ! test -f /ctx/debug.log "
                       << "&& ! test -d /ctx/temp "
                       << "&& echo dockerignore_ok\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build-dockerignore:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-dockerignore:latest");

        WSLCContainerLauncher launcher("wslc-test-build-dockerignore:latest", "wslc-build-dockerignore-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("dockerignore_ok") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageFailure)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-failure";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM does-not-exist:invalid\n";
        }

        VERIFY_FAILED(BuildImageFromContext(contextDir, "wslc-test-build-failure:latest"));
        auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
        VERIFY_IS_TRUE(comError.has_value());
        LogInfo("Expected build error: %ls", comError->Message.get());

        ExpectImagePresent(*m_defaultSession, "wslc-test-build-failure:latest", false);
    }

    WSLC_TEST_METHOD(BuildImageFailureShowsBuildOutput)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-failure-output";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-args:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "RUN echo 'build-log-marker' && /bin/false\n";
        }

        class ProgressAccumulator
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
        {
        public:
            ProgressAccumulator(std::string& output) : m_output(output)
            {
            }
            HRESULT OnProgress(LPCSTR message, LPCSTR, ULONGLONG, ULONGLONG) override
            {
                if (message)
                {
                    m_output.append(message);
                }
                return S_OK;
            }

        private:
            std::string& m_output;
        };

        std::string progressOutput;
        auto callback = Microsoft::WRL::Make<ProgressAccumulator>(progressOutput);

        auto dockerfileHandle = wil::open_file((contextDir / "Dockerfile").c_str());
        auto contextPathStr = contextDir.wstring();
        LPCSTR tag = "wslc-test-build-failure-output:latest";
        WSLCBuildImageOptions options{
            .ContextPath = contextPathStr.c_str(),
            .DockerfileHandle = ToCOMInputHandle(dockerfileHandle.get()),
            .Tags = {&tag, 1},
        };

        VERIFY_FAILED(m_defaultSession->BuildImage(&options, callback.Get(), nullptr));
        VERIFY_IS_TRUE(progressOutput.find("build-log-marker") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageStdinDockerfile)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-stdin";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-stdin:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        auto dockerfileContent = "FROM debian:latest\nCMD [\"echo\", \"stdin-dockerfile-ok\"]\n";

        wil::unique_hfile readHandle;
        wil::unique_hfile writeHandle;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(readHandle.addressof(), writeHandle.addressof(), nullptr, 0));

        DWORD bytesWritten;
        THROW_IF_WIN32_BOOL_FALSE(
            WriteFile(writeHandle.get(), dockerfileContent, static_cast<DWORD>(strlen(dockerfileContent)), &bytesWritten, nullptr));
        writeHandle.reset();

        auto contextPathStr = contextDir.wstring();
        LPCSTR tag = "wslc-test-build-stdin:latest";
        WSLCBuildImageOptions options{
            .ContextPath = contextPathStr.c_str(),
            .DockerfileHandle = ToCOMInputHandle(readHandle.get()),
            .Tags = {&tag, 1},
        };
        VERIFY_SUCCEEDED(m_defaultSession->BuildImage(&options, nullptr, nullptr));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-stdin:latest");

        WSLCContainerLauncher launcher("wslc-test-build-stdin:latest", "wslc-build-stdin-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("stdin-dockerfile-ok") != std::string::npos);
    }

    WSLC_TEST_METHOD(BuildImageBuildArgs)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-buildargs";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build-args:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "ARG TEST_VALUE\n";
            dockerfile << "ENV TEST_VALUE=${TEST_VALUE}\n";
            dockerfile << "CMD echo \"build-arg-value=${TEST_VALUE}\"\n";
        }

        LPCSTR tag = "wslc-test-build-args:latest";
        LPCSTR buildArg = "TEST_VALUE=hello-from-build-arg";
        WSLCBuildImageOptions options{.Tags = {&tag, 1}, .BuildArgs = {&buildArg, 1}};
        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, &options));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-args:latest");

        WSLCContainerLauncher launcher("wslc-test-build-args:latest", "wslc-build-args-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto initProcess = container.GetInitProcess();
        ValidateProcessOutput(initProcess, {{1, "build-arg-value=hello-from-build-arg\n"}});
    }

    WSLC_TEST_METHOD(BuildImageMultipleTags)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-multitag";
        std::filesystem::create_directories(contextDir);
        LPCSTR tags[] = {"wslc-test-multitag:v1", "wslc-test-multitag:v2"};
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            for (auto* tag : tags)
            {
                LOG_IF_FAILED(DeleteImageNoThrow(tag, WSLCDeleteImageFlagsForce).first);
            }

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "CMD [\"echo\", \"multi-tag-ok\"]\n";
        }
        WSLCBuildImageOptions options{.Tags = {tags, 2}};
        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, &options));
        ExpectImagePresent(*m_defaultSession, "wslc-test-multitag:v1");
        ExpectImagePresent(*m_defaultSession, "wslc-test-multitag:v2");
    }

    WSLC_TEST_METHOD(BuildImageNullHandle)
    {
        WSLCBuildImageOptions options{.ContextPath = L"C:\\", .DockerfileHandle = {}, .Tags = {nullptr, 0}};

        VERIFY_ARE_EQUAL(m_defaultSession->BuildImage(&options, nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
    }

    WSLC_TEST_METHOD(BuildImageCancel)
    {
        class TestProgressCallback
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
        {
        public:
            TestProgressCallback(wil::unique_event& event) : m_event(event)
            {
            }

            HRESULT OnProgress(LPCSTR, LPCSTR, ULONGLONG, ULONGLONG) override
            {
                m_event.SetEvent();
                return S_OK;
            }

        private:
            wil::unique_event& m_event;
        };

        auto contextDir = std::filesystem::current_path() / "build-context-cancel";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        // Use a Dockerfile that takes a long time to build so we can cancel it mid-build.
        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "RUN sleep 120\n";
        }

        wil::unique_event cancelEvent{wil::EventOptions::ManualReset};
        wil::unique_event progressEvent{wil::EventOptions::ManualReset};

        // Use a progress callback to detect when the build is actively running
        // before signaling cancellation, avoiding a racy Sleep().
        auto callback = Microsoft::WRL::Make<TestProgressCallback>(progressEvent);

        auto contextPathStr = contextDir.wstring();
        auto dockerfileHandle = wil::open_file((contextDir / "Dockerfile").c_str());

        LPCSTR tag = "wslc-test-build-cancel:latest";
        WSLCBuildImageOptions options{
            .ContextPath = contextPathStr.c_str(), .DockerfileHandle = ToCOMInputHandle(dockerfileHandle.get()), .Tags = {&tag, 1}};

        std::promise<HRESULT> result;
        std::thread buildThread(
            [&]() { result.set_value(m_defaultSession->BuildImage(&options, callback.Get(), cancelEvent.get())); });

        auto joinThread = wil::scope_exit([&]() { buildThread.join(); });

        VERIFY_IS_TRUE(progressEvent.wait(60 * 1000));
        cancelEvent.SetEvent();

        VERIFY_ARE_EQUAL(E_ABORT, result.get_future().get());
    }

    WSLC_TEST_METHOD(BuildImageNoCache)
    {
        auto contextDir = std::filesystem::current_path() / "build-context-nocache";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-nocache:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "RUN echo -n Image && echo -n is && echo -n rebuilt\n";
        }

        // First build to populate cache.
        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-nocache:latest"));

        // Validate that the image isn't rebuilt when NoCache isn't set.
        {
            std::string output;
            auto callback = Microsoft::WRL::Make<CapturingProgressCallback>(output);
            LPCSTR tag = "wslc-test-nocache:latest";
            WSLCBuildImageOptions options{.Tags = {&tag, 1}};
            VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, &options, callback.Get()));
            VERIFY_IS_TRUE(output.find("Imageisrebuilt") == std::string::npos);
        }

        // Validate that the image is rebuilt when WSLCBuildImageFlagsNoCache is set, and that the output from the RUN step appears in the progress callback.
        {
            std::string output;
            auto callback = Microsoft::WRL::Make<CapturingProgressCallback>(output);
            LPCSTR tag = "wslc-test-nocache:latest";
            WSLCBuildImageOptions options{.Tags = {&tag, 1}, .Flags = WSLCBuildImageFlagsNoCache};
            VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, &options, callback.Get()));
            VERIFY_IS_TRUE(output.find("Imageisrebuilt") != std::string::npos);
        }
    }

    WSLC_TEST_METHOD(BuildImageInvalidFlags)
    {
        auto dummyDockerfile = wil::create_new_file(
            (std::filesystem::current_path() / "Dockerfile").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, FILE_FLAG_DELETE_ON_CLOSE);

        auto contextDir = std::filesystem::current_path();

        WSLCBuildImageOptions options{
            .ContextPath = contextDir.c_str(),
            .DockerfileHandle = ToCOMInputHandle(dummyDockerfile.get()),
            .Flags = static_cast<WSLCBuildImageFlags>(0x10)};

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->BuildImage(&options, nullptr, nullptr));
    }
};
