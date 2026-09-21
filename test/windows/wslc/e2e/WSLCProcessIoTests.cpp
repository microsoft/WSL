/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessIoTests.cpp

Abstract:

    This file contains test cases for WSLC process creation and IO redirection.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCProcessIoTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCProcessIoTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    WSLC_TEST_METHOD(SynchronousIoCancellation)
    {
        // Create a blocked operation that will cause the service to get stuck on a ReadFile() call.
        // Because the pipe handle that we're passing in doesn't support overlapped IO, the service will get stuck in a
        // synchronous ReadFile() call. Validate that terminating the session correctly cancels the IO.

        wil::unique_handle pipeRead;
        wil::unique_handle pipeWrite;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

        std::promise<HRESULT> result;

        wil::unique_event testCompleted{wil::EventOptions::ManualReset};
        std::thread operationThread([&]() {
            wil::unique_cotaskmem_ansistring id;
            result.set_value(m_defaultSession->ImportImage(ToCOMInputHandle(pipeRead.get()), "dummy:latest", 1024 * 1024, nullptr, &id));

            WI_ASSERT(testCompleted.is_signaled()); // Sanity check.
        });

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

        // Write 4 bytes to validate that the service has started reading from the pipe (since the pipe buffer is 2).
        DWORD bytesWritten{};
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

        testCompleted.SetEvent();

        // N.B. It's not possible to deterministically wait for the service to be stuck in the ReadFile() call.
        // It's possible that the service will check the session termination event before calling ReadFile() on the pipe
        // but that's OK since we can also accept that error code here (E_ABORT).
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());

        auto reset = ResetTestSession();

        auto hr = result.get_future().get();
        if (hr != E_ABORT && hr != HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED))
        {
            LogError("Unexpected result: 0x%08X", hr);
            VERIFY_FAIL();
        }
    }

    WSLC_TEST_METHOD(ExportContainer)
    {
        // Load an image and launch a container to verify image is valid.
        // Then export the container to a tar file.
        // Load the exported tar file to verify it's a valid image and can be launched.
        // Finally, stop and delete the container, then try to export again to verify it fails as expected.
        {
            std::filesystem::path containerTar = L"HelloWorldExported.tar";
            auto cleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(containerTar.c_str())); });

            // Load the image from a saved tar and launch a container
            {
                std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
                wil::unique_handle imageTarFileHandle{CreateFileW(
                    imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), fileSize.QuadPart, nullptr, nullptr));
                // Verify that the image is in the list of images.
                ExpectImagePresent(*m_defaultSession, "hello-world:latest");
                WSLCContainerLauncher launcher("hello-world:latest", "wslc-hello-world-container");
                auto container = launcher.Launch(*m_defaultSession);
                auto result = container.GetInitProcess().WaitAndCaptureOutput();
                VERIFY_ARE_EQUAL(0, result.Code);
                VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

                // Export the container to a tar file.
                wil::unique_handle containerTarFileHandle{CreateFileW(
                    containerTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == containerTarFileHandle.get());
                VERIFY_IS_TRUE(GetFileSizeEx(containerTarFileHandle.get(), &fileSize));
                VERIFY_ARE_EQUAL(fileSize.QuadPart, 0);
                VERIFY_SUCCEEDED(container.Get().Export(ToCOMInputHandle(containerTarFileHandle.get())));
                VERIFY_IS_TRUE(GetFileSizeEx(containerTarFileHandle.get(), &fileSize));
                VERIFY_ARE_NOT_EQUAL(fileSize.QuadPart, 0);
            }

            // Load the exported container to verify it's valid.
            {
                wil::unique_handle containerTarFileHandle{CreateFileW(
                    containerTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == containerTarFileHandle.get());
                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(containerTarFileHandle.get(), &fileSize));

                auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                    LOG_IF_FAILED(DeleteImageNoThrow("test-imported-container:latest", WSLCDeleteImageFlagsNone).first);
                });

                wil::unique_cotaskmem_ansistring importedImageId;
                VERIFY_SUCCEEDED(m_defaultSession->ImportImage(
                    ToCOMInputHandle(containerTarFileHandle.get()), "test-imported-container:latest", fileSize.QuadPart, nullptr, &importedImageId));

                // Verify that the image is in the list of images.
                ExpectImagePresent(*m_defaultSession, "test-imported-container:latest");
                WSLCContainerLauncher launcher("test-imported-container:latest", "wslc-hello-world-container", {"/hello"});
                auto container = launcher.Launch(*m_defaultSession);
                auto result = container.GetInitProcess().WaitAndCaptureOutput();
                VERIFY_ARE_EQUAL(0, result.Code);
                VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

                // Stop and delete the above container and try to export.

                std::filesystem::path imageTarFile = L"HelloWorldExportError.tar";
                auto cleanfile =
                    wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTarFile.c_str())); });
                wil::unique_handle contTarFileHandle{CreateFileW(
                    imageTarFile.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == contTarFileHandle.get());
                VERIFY_IS_TRUE(GetFileSizeEx(contTarFileHandle.get(), &fileSize));
                VERIFY_ARE_EQUAL(fileSize.QuadPart, 0);

                auto outFile = ToCOMInputHandle(contTarFileHandle.get());

                container.Get().Stop(WSLCSignalSIGILL, 10);
                container.Get().Delete(WSLCDeleteFlagsNone);
                VERIFY_ARE_EQUAL(container.Get().Export(outFile), RPC_E_DISCONNECTED);

                VERIFY_IS_TRUE(GetFileSizeEx(contTarFileHandle.get(), &fileSize));
                VERIFY_ARE_EQUAL(fileSize.QuadPart, 0);
            }
        }
    }

    WSLC_TEST_METHOD(CustomDmesgOutput)
    {
        SKIP_TEST_ARM64();

        auto createVmWithDmesg = [this](bool earlyBootLogging) {
            auto [read, write] = CreateSubprocessPipe(false, false);

            auto settings = GetDefaultSessionSettings(L"dmesg-output-test");
            settings.DmesgOutput = ToCOMInputHandle(write.get());
            WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsEarlyBootDmesg, earlyBootLogging);

            std::vector<char> dmesgContent;
            auto readDmesg = [read = read.get(), &dmesgContent]() mutable {
                DWORD Offset = 0;

                constexpr auto bufferSize = 1024;
                while (true)
                {
                    dmesgContent.resize(Offset + bufferSize);

                    DWORD Read{};
                    if (!ReadFile(read, &dmesgContent[Offset], bufferSize, &Read, nullptr))
                    {
                        LogInfo("ReadFile() failed: %lu", GetLastError());
                    }

                    if (Read == 0)
                    {
                        break;
                    }

                    Offset += Read;
                }
            };

            std::thread thread(readDmesg); // Needs to be created before the VM starts, to avoid a pipe deadlock.

            // Ensure the thread is joined even if CreateSession throws, to avoid std::terminate.
            auto threadGuard = wil::scope_exit([&]() {
                write.reset();
                if (thread.joinable())
                {
                    thread.join();
                }
            });

            auto session = CreateSession(settings);
            threadGuard.release(); // CreateSession succeeded, detach scope_exit below takes over.

            auto detach = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                session.reset();
                if (thread.joinable())
                {
                    thread.join();
                }
            });

            write.reset();

            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo DmesgTest > /dev/kmsg"}, 0);

            session.reset();
            detach.reset();

            auto contentString = std::string(dmesgContent.begin(), dmesgContent.end());

            VERIFY_ARE_NOT_EQUAL(contentString.find("Run /init as init process"), std::string::npos);
            VERIFY_ARE_NOT_EQUAL(contentString.find("DmesgTest"), std::string::npos);

            return contentString;
        };

        auto validateFirstDmesgLine = [](const std::string& dmesg, const char* expected) {
            auto firstLf = dmesg.find("\n");
            VERIFY_ARE_NOT_EQUAL(firstLf, std::string::npos);
            VERIFY_IS_TRUE(dmesg.find(expected) < firstLf);
        };

        // Dmesg without early boot logging
        {
            auto dmesg = createVmWithDmesg(false);

            // Verify that the first line is "brd: module loaded";
            validateFirstDmesgLine(dmesg, "brd: module loaded");
        }

        // Dmesg with early boot logging
        {
            auto dmesg = createVmWithDmesg(true);
            validateFirstDmesgLine(dmesg, "Linux version");
        }
    }

    WSLC_TEST_METHOD(TerminationEvent)
    {
        auto session = CreateSession(GetDefaultSessionSettings(L"termination-event-test"));

        wil::unique_handle terminationEvent;
        VERIFY_SUCCEEDED(session->GetTerminationEvent(&terminationEvent));
        VERIFY_IS_NOT_NULL(terminationEvent.get());

        // The reason is unavailable until the session has terminated.
        WSLCVirtualMachineTerminationReason reason{};
        wil::unique_cotaskmem_string details;
        VERIFY_ARE_EQUAL(session->GetTerminationReason(&reason, &details), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

        // Terminating the session should signal the event and record a graceful shutdown reason.
        VERIFY_SUCCEEDED(session->Terminate());

        VERIFY_ARE_EQUAL(WaitForSingleObject(terminationEvent.get(), 30 * 1000), static_cast<DWORD>(WAIT_OBJECT_0));

        VERIFY_SUCCEEDED(session->GetTerminationReason(&reason, &details));
        VERIFY_ARE_EQUAL(reason, WSLCVirtualMachineTerminationReasonShutdown);
    }

    WSLC_TEST_METHOD(CrashDumpCallback)
    {
        struct Invocation
        {
            std::wstring DumpPath;
            std::string ProcessName;
            ULONG Pid;
            ULONG Signal;
            ULONGLONG Timestamp;
        };

        class DECLSPEC_UUID("8C5A7B14-9D26-4FAE-AB31-7E5BC23F4802") CallbackInstance
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICrashDumpCallback, IFastRundown, Microsoft::WRL::FtmBase>
        {
        public:
            CallbackInstance(std::promise<Invocation>& promise, wil::unique_event& release) :
                m_promise(promise), m_release(release)
            {
            }

            HRESULT OnCrashDump(LPCWSTR DumpPath, LPCSTR ProcessName, ULONG Pid, ULONG Signal, ULONGLONG Timestamp) override
            {
                m_promise.set_value(Invocation{
                    DumpPath ? std::wstring{DumpPath} : std::wstring{}, ProcessName ? std::string{ProcessName} : std::string{}, Pid, Signal, Timestamp});

                // Block until the test has finished probing, so anything the test verifies is observed mid-callback.
                m_release.wait();
                return S_OK;
            }

        private:
            std::promise<Invocation>& m_promise;
            wil::unique_event& m_release;
        };

        std::promise<Invocation> promise;
        wil::unique_event release{wil::EventOptions::ManualReset};
        auto callback = Microsoft::WRL::Make<CallbackInstance>(promise, release);
        auto releaseCallback = wil::scope_exit([&]() { release.SetEvent(); });

        WSLCSessionSettings sessionSettings = GetDefaultSessionSettings(L"crash-dump-callback-test");
        auto session = CreateSession(sessionSettings);

        // Register the callback through IWSLCSession::RegisterCrashDumpCallback. Holding the
        // returned subscription keeps the registration alive; releasing it auto-unregisters.
        wil::com_ptr<IUnknown> subscription;
        VERIFY_SUCCEEDED(session->RegisterCrashDumpCallback(callback.Get(), &subscription));

        // Trigger a Linux process crash. The shell exits with 128 + SIGSEGV.
        ExpectCommandResult(session.get(), {"/bin/sh", "-c", "kill -SEGV $$"}, 128 + WSLCSignalSIGSEGV);

        auto future = promise.get_future();
        VERIFY_ARE_EQUAL(future.wait_for(std::chrono::seconds(60)), std::future_status::ready);

        auto invocation = future.get();
        VERIFY_IS_FALSE(invocation.DumpPath.empty());
        VERIFY_IS_TRUE(invocation.ProcessName.find("sh") != std::string::npos);
        VERIFY_ARE_EQUAL(invocation.Signal, static_cast<ULONG>(WSLCSignalSIGSEGV));
        VERIFY_IS_GREATER_THAN(invocation.Pid, 0u);
        VERIFY_IS_GREATER_THAN(invocation.Timestamp, 0ull);

        // The dump file should be readable and non-empty.
        wil::unique_hfile dumpFile{CreateFileW(
            invocation.DumpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_TRUE(dumpFile.is_valid());
        VERIFY_IS_GREATER_THAN(std::filesystem::file_size(invocation.DumpPath), 0ull);
    }

    WSLC_TEST_METHOD(BuildImageStuckCallbackCancellation)
    {
        SKIP_TEST_SERVER();

        class StuckBuildProgressCallback
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
        {
        public:
            StuckBuildProgressCallback(std::promise<void>& reachedPromise, wil::unique_event& exitEvent) :
                m_reachedPromise(reachedPromise), m_exitEvent(exitEvent)
            {
            }

            HRESULT OnProgress(LPCSTR, LPCSTR, ULONGLONG, ULONGLONG) override
            {
                if (!m_signaled)
                {
                    m_signaled = true;
                    m_reachedPromise.set_value();
                    m_exitEvent.wait(); // Block until this test case is complete.
                }

                return S_OK;
            }

        private:
            std::promise<void>& m_reachedPromise;
            wil::unique_event& m_exitEvent;
            bool m_signaled{};
        };

        auto contextDir = std::filesystem::current_path() / "build-context-stuck-callback";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "RUN echo hello\n";
        }

        auto contextPathStr = contextDir.wstring();
        auto dockerfileHandle = wil::open_file((contextDir / "Dockerfile").c_str());

        WSLCBuildImageOptions options{
            .ContextPath = contextPathStr.c_str(),
            .DockerfileHandle = ToCOMInputHandle(dockerfileHandle.get()),
            .Flags = WSLCBuildImageFlagsVerbose,
        };

        std::promise<void> callbackReached;
        wil::unique_event exitEvent{wil::EventOptions::ManualReset};
        auto callback = Microsoft::WRL::Make<StuckBuildProgressCallback>(callbackReached, exitEvent);

        std::promise<HRESULT> buildResult;
        std::thread buildThread(
            [&]() { buildResult.set_value(m_defaultSession->BuildImage(&options, callback.Get(), exitEvent.get())); });

        auto joinThread = wil::scope_exit([&]() {
            exitEvent.SetEvent();
            buildThread.join();
        });

        // Wait for the progress callback to be called, proving the COM call is in flight.
        auto reachedFuture = callbackReached.get_future();
        auto reachedStatus = reachedFuture.wait_for(std::chrono::seconds(60));
        VERIFY_ARE_EQUAL(reachedStatus, std::future_status::ready);

        // Terminate the session while the callback is stuck.
        // This should cancel the pending COM call and unblock BuildImage.
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        ResetTestSession();

        auto buildFuture = buildResult.get_future();
        auto buildStatus = buildFuture.wait_for(std::chrono::seconds(60));
        VERIFY_ARE_EQUAL(buildStatus, std::future_status::ready);

        // BuildImage should have failed due to COM call cancellation.
        VERIFY_FAILED(buildFuture.get());
    }

    WSLC_TEST_METHOD(InteractiveShell)
    {
        WSLCProcessLauncher launcher("/bin/sh", {"/bin/sh"}, {"TERM=xterm-256color"}, WSLCProcessFlagsTty | WSLCProcessFlagsStdin);
        auto process = launcher.Launch(*m_defaultSession);

        auto tty = process.GetStdHandle(WSLCFDTty);

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(tty.Get(), buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(tty.Get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("\033[?2004hsh-5.2# ");
        writeTty("echo OK\n");
        validateTtyOutput("echo OK\r\n\033[?2004l\rOK");

        // Exit the shell
        writeTty("exit\n");

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 1000));
    }
};
