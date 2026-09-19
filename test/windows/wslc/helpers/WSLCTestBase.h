/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTestBase.h

Abstract:

    Shared fixture and helpers for the WSLC API test classes.

--*/

#pragma once

#include "Common.h"
#include "wslc.h"
#include "wslccompat.h"
#include "WSLCProcessLauncher.h"
#include "WSLCContainerLauncher.h"
#include "WSLCContainerEntry.h"
#include "WslCoreFilesystem.h"
#include "hcs.hpp"
#include "ContainerNameGenerator.h"
#include "WSLCE2EHelpers.h"
#include "HttpHeaderEndDetector.h"
#include "WSLCSessionDefaults.h"
#include <nlohmann/json.hpp>

using namespace std::chrono;
using namespace std::literals::chrono_literals;
using namespace wsl::windows::common::registry;
using wsl::windows::common::ClientRunningWSLCProcess;
using wsl::windows::common::RunningWSLCContainer;
using wsl::windows::common::RunningWSLCProcess;
using wsl::windows::common::WSLCContainerLauncher;
using wsl::windows::common::WSLCProcessLauncher;
using wsl::windows::common::io::OverlappedIOHandle;
using wsl::windows::common::io::WriteHandle;
using namespace wsl::windows::common::wslutil;
using WSLCE2ETests::StartLocalRegistry;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

//
// State that is shared by every WSLC API test class. Creating the session and loading the test
// images is expensive, so it is done once per test run and torn down from ModuleCleanup.
//
struct WSLCTestFixture
{
    static WSLCTestFixture& Instance();

    bool Initialized = false;
    WSADATA Wsadata{};
    std::filesystem::path StoragePath;
    WSLCSessionSettings DefaultSessionSettings{};
    wil::com_ptr<IWSLCSession> DefaultSession;
};

void WSLCTestFixtureCleanup();

class WSLCTestBase
{
protected:
    std::filesystem::path& m_storagePath = WSLCTestFixture::Instance().StoragePath;
    WSLCSessionSettings& m_defaultSessionSettings = WSLCTestFixture::Instance().DefaultSessionSettings;
    wil::com_ptr<IWSLCSession>& m_defaultSession = WSLCTestFixture::Instance().DefaultSession;
    static inline auto c_testSessionName = L"wslc-test";

    bool BaseClassSetup()
    {
        auto& fixture = WSLCTestFixture::Instance();
        if (!fixture.Initialized)
        {
            THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &fixture.Wsadata));

            // The WSLC SDK tests use this same storage to reduce pull overhead.
            m_storagePath = std::filesystem::current_path() / "test-storage";
            m_defaultSessionSettings = GetDefaultSessionSettings(c_testSessionName, true, WSLCNetworkingModeConsomme);
            fixture.Initialized = true;
        }

        m_defaultSession = CreateSession(m_defaultSessionSettings);

        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, &images, images.size_address<ULONG>()));

        auto hasImage = [&](const std::string& imageName) {
            return std::ranges::any_of(
                images.get(), images.get() + images.size(), [&](const auto& e) { return e.Image == imageName; });
        };

        if (!hasImage("debian:latest"))
        {
            LoadTestImage(*m_defaultSession, "debian:latest");
        }

        if (!hasImage("python:3.12-alpine"))
        {
            LoadTestImage(*m_defaultSession, "python:3.12-alpine");
        }

        if (!hasImage("hello-world:latest"))
        {
            LoadTestImage(*m_defaultSession, "hello-world:latest");
        }

        if (!hasImage("alpine:latest"))
        {
            LoadTestImage(*m_defaultSession, "alpine:latest");
        }

        if (!hasImage("wslc-registry:latest"))
        {
            LoadTestImage(*m_defaultSession, "wslc-registry:latest");
        }

        PruneResult result;
        VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));
        if (result.result.ContainersCount > 0)
        {
            LogInfo("Pruned %lu containers", result.result.ContainersCount);
        }

        return true;
    }

    bool BaseClassCleanup()
    {
        // The session is released between test classes so the other classes that use the same
        // session name and storage can create their own. The storage itself is kept for the
        // lifetime of the test run and removed by WSLCTestFixtureCleanup().
        m_defaultSession.reset();

        return true;
    }

    WSLCSessionSettings GetDefaultSessionSettings(LPCWSTR Name, bool enableStorage = false, WSLCNetworkingMode networkingMode = WSLCNetworkingModeNone)
    {
        WSLCSessionSettings settings{};
        settings.DisplayName = Name;
        settings.CpuCount = 4;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.StoragePath = enableStorage ? m_storagePath.c_str() : nullptr;
        settings.MaximumStorageSizeMb = 1024 * 20; // 20GB.
        settings.NetworkingMode = networkingMode;

        return settings;
    }

    auto ResetTestSession()
    {
        m_defaultSession.reset();

        return wil::scope_exit([this]() { m_defaultSession = CreateSession(m_defaultSessionSettings); });
    }

    static wil::com_ptr<IWSLCSessionManager> OpenSessionManager()
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        return sessionManager;
    }

    // Returns true for the names the wslc CLI reserves for its default sessions.
    static bool IsCliSessionName(std::wstring_view Name)
    {
        constexpr std::wstring_view prefix{wsl::windows::wslc::DefaultSessionName};

        return Name.size() >= prefix.size() && wsl::shared::string::IsEqual(Name.substr(0, prefix.size()), prefix, true) &&
               (Name.size() == prefix.size() || Name[prefix.size()] == L'-');
    }

    // ListSessions() reports every session on the machine, including the persistent sessions the
    // wslc CLI creates for itself. Those are outside this class's control, so they are filtered
    // out to keep assertions independent of what else has run on the machine.
    static std::set<std::wstring> ListTestSessionNames(IWSLCSessionManager* SessionManager)
    {
        wil::unique_cotaskmem_array_ptr<WSLCSessionListEntry> sessions;
        VERIFY_SUCCEEDED(SessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

        std::set<std::wstring> names;
        for (const auto& e : sessions)
        {
            if (IsCliSessionName(e.DisplayName))
            {
                continue;
            }

            auto [it, inserted] = names.emplace(e.DisplayName);
            VERIFY_IS_TRUE(inserted);
        }

        return names;
    }

    wil::com_ptr<IWSLCSession> CreateSession(const WSLCSessionSettings& sessionSettings, WSLCSessionFlags Flags = WSLCSessionFlagsNone)
    {
        const auto sessionManager = OpenSessionManager();

        wil::com_ptr<IWSLCSession> session;

        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, nullptr, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        WSLCSessionState state{};
        VERIFY_SUCCEEDED(session->GetState(&state));
        VERIFY_ARE_EQUAL(state, WSLCSessionStateRunning);

        return session;
    }

    RunningWSLCContainer OpenContainer(IWSLCSession* session, const std::string& name)
    {
        wil::com_ptr<IWSLCContainer> rawContainer;
        VERIFY_SUCCEEDED(session->OpenContainer(name.c_str(), &rawContainer));

        return RunningWSLCContainer(std::move(rawContainer), {});
    }

    struct ListContainersResult
    {
        wsl::windows::common::wslc::unique_container_entry_array Containers;
        wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> Ports;
    };

    // Issues IWSLCSession::ListContainers with WSLCListContainersFlagsAll (all containers, no filter).
    // If a future caller needs a different flag set, add a parameter.
    ListContainersResult ListContainers(IWSLCSession* session)
    {
        WSLCListContainersOptions options{};
        options.Flags = WSLCListContainersFlagsAll;

        ListContainersResult result;
        VERIFY_SUCCEEDED(session->ListContainers(
            &options,
            result.Containers.addressof(),
            result.Containers.size_address<ULONG>(),
            result.Ports.addressof(),
            result.Ports.size_address<ULONG>()));

        return result;
    }

    static RunningWSLCProcess::ProcessResult RunCommand(IWSLCSession* session, const std::vector<std::string>& command, int timeout = 600000)
    {
        WSLCProcessLauncher process(command[0], command);

        return process.Launch(*session).WaitAndCaptureOutput(timeout);
    }

    static RunningWSLCProcess::ProcessResult ExpectCommandResult(
        IWSLCSession* session, const std::vector<std::string>& command, int expectResult, int timeout = 600000)
    {
        auto result = RunCommand(session, command, timeout);

        if (result.Code != expectResult)
        {
            auto cmd = wsl::shared::string::Join(command, ' ');
            LogError(
                "Command: %hs didn't return expected code (%i). ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                cmd.c_str(),
                expectResult,
                result.Code,
                result.Output[1].c_str(),
                result.Output[2].c_str());
        }

        return result;
    }

    void ValidateProcessOutput(RunningWSLCProcess& process, const std::map<int, std::string>& expectedOutput, int expectedResult = 0, DWORD Timeout = INFINITE)
    {
        auto result = process.WaitAndCaptureOutput(Timeout);

        if (result.Code != expectedResult)
        {
            LogError(
                "Command didn't return expected code (%i). ExitCode: %i, Stdout: '%hs', Stderr: '%hs'",
                expectedResult,
                result.Code,
                EscapeString(result.Output[1]).c_str(),
                EscapeString(result.Output[2]).c_str());

            return;
        }

        for (const auto& [fd, expected] : expectedOutput)
        {
            auto it = result.Output.find(fd);
            if (it == result.Output.end())
            {
                LogError("Expected output on fd %i, but none found.", fd);
                return;
            }

            if (it->second != expected)
            {
                LogError(
                    "Unexpected output on fd %i. Expected: '%hs', Actual: '%hs'",
                    fd,
                    EscapeString(expected).c_str(),
                    EscapeString(it->second).c_str());

                return;
            }
        }
    }

    void ValidateContainerOutput(RunningWSLCContainer& container, const std::map<int, std::string>& expectedOutput, int expectedResult = 0, DWORD timeout = INFINITE)
    {
        auto initProcess = container.GetInitProcess();
        ValidateProcessOutput(initProcess, expectedOutput, expectedResult, timeout);
    }

    void ValidateContainerOutput(WSLCContainerLauncher& launcher, const std::map<int, std::string>& expectedOutput, int expectedResult = 0, DWORD timeout = INFINITE)
    {
        auto container = launcher.Launch(*m_defaultSession);
        ValidateContainerOutput(container, expectedOutput, expectedResult, timeout);
    }

    void ExpectMount(IWSLCSession* session, const std::string& target, const std::optional<std::string>& options)
    {
        auto cmd = std::format("set -o pipefail ; findmnt '{}' | tail  -n 1", target);
        auto result = ExpectCommandResult(session, {"/bin/sh", "-c", cmd}, options.has_value() ? 0 : 1);

        const auto& output = result.Output[1];
        const auto& error = result.Output[2];

        if (result.Code != (options.has_value() ? 0 : 1))
        {
            LogError("%hs failed. code=%i, output: %hs, error: %hs", cmd.c_str(), result.Code, output.c_str(), error.c_str());
            VERIFY_FAIL();
        }

        if (options.has_value() && !PathMatchSpecA(output.c_str(), options->c_str()))
        {
            std::wstring message = std::format(L"Output: '{}' didn't match pattern: '{}'", output, options.value());
            VERIFY_FAIL(message.c_str());
        }
    }

    struct VmInfo
    {
        std::wstring Id;
        std::wstring Owner;
    };

    // Returns VM info (Id + Owner) for all compute systems via the HCS API.
    static std::vector<VmInfo> ListVms()
    {
        const wsl::windows::common::ExecutionContext context(wsl::windows::common::Context::HCS);

        auto operation = wsl::windows::common::hcs::CreateOperation();
        THROW_IF_FAILED(::HcsEnumerateComputeSystems(L"{}", operation.get()));

        wil::unique_cotaskmem_string resultDocument;
        const auto result = ::HcsWaitForOperationResult(operation.get(), 10000, &resultDocument);
        THROW_IF_FAILED_MSG(result, "HcsEnumerateComputeSystems failed (error: %ls)", resultDocument.get());

        LogInfo("HcsEnumerateComputeSystems result='%ws'", resultDocument.get());

        std::vector<VmInfo> vms;
        const auto json = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(resultDocument.get()));
        if (!json.is_array())
        {
            return vms;
        }

        for (const auto& entry : json)
        {
            if (entry.contains("Owner") && entry["Owner"].is_string() && entry.contains("Id") && entry["Id"].is_string())
            {
                vms.push_back(
                    {wsl::shared::string::MultiByteToWide(entry["Id"].get<std::string>()),
                     wsl::shared::string::MultiByteToWide(entry["Owner"].get<std::string>())});
            }
        }

        return vms;
    }

    void ExpectImagePresent(IWSLCSession& Session, const char* Image, bool Present = true)
    {
        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        THROW_IF_FAILED(Session.ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

        std::vector<std::string> tags;
        for (const auto& e : images)
        {
            tags.push_back(e.Image);
        }

        auto found = std::ranges::find(tags, Image) != tags.end();
        if (Present != found)
        {
            LogError("Image presence check failed for image: %hs, images: %hs", Image, wsl::shared::string::Join(tags, ',').c_str());
            VERIFY_FAIL();
        }
    }

    std::pair<HRESULT, wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation>> DeleteImageNoThrow(const std::string& Image, DWORD Flags)
    {
        WSLCDeleteImageOptions options{};
        options.Image = Image.c_str();
        options.Flags = Flags;
        wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
        auto hr = m_defaultSession->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>());
        return {hr, std::move(deletedImages)};
    }

    wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> DeleteImage(const std::string& Image, DWORD Flags)
    {
        auto [hr, deletedImages] = DeleteImageNoThrow(Image, Flags);
        VERIFY_SUCCEEDED(hr);

        return std::move(deletedImages);
    }

    std::vector<wsl::windows::common::wslc_schema::VolumeListEntry> ListVolumeEntries(const std::vector<WSLCFilter>& Filters = {})
    {
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(Filters.empty() ? nullptr : Filters.data(), static_cast<ULONG>(Filters.size()), &output));

        return wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::VolumeListEntry>>(output.get());
    }

    std::set<std::string> ListVolumes(const std::vector<WSLCFilter>& Filters = {})
    {
        std::set<std::string> names;
        for (const auto& v : ListVolumeEntries(Filters))
        {
            names.insert(v.Name);
        }
        return names;
    }

    void CreateNamedVolume(
        const std::string& Name,
        const std::string& Driver,
        const std::vector<WSLCLabel>& Labels = {},
        const std::vector<WSLCDriverOption>& DriverOpts = {})
    {
        WSLCVolumeOptions options{};
        options.Name = Name.c_str();
        options.Driver = Driver.c_str();
        options.DriverOpts = DriverOpts.empty() ? nullptr : DriverOpts.data();
        options.DriverOptsCount = static_cast<ULONG>(DriverOpts.size());
        options.Labels = Labels.empty() ? nullptr : Labels.data();
        options.LabelsCount = static_cast<ULONG>(Labels.size());

        WSLCVolumeInformation info{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&options, &info));
    }

    HRESULT BuildImageFromContext(const std::filesystem::path& contextDir, const WSLCBuildImageOptions* options, IProgressCallback* callback = nullptr)
    {
        auto dockerfileHandle = wil::open_file((contextDir / "Dockerfile").c_str());

        auto contextPathStr = contextDir.wstring();
        WSLCBuildImageOptions optionsCopy = *options;
        optionsCopy.ContextPath = contextPathStr.c_str();
        optionsCopy.DockerfileHandle = ToCOMInputHandle(dockerfileHandle.get());

        auto buildResult = m_defaultSession->BuildImage(&optionsCopy, callback, nullptr);

        if (FAILED(buildResult))
        {
            LogInfo("BuildImage failed: 0x%08x", buildResult);
        }

        return buildResult;
    }

    HRESULT BuildImageFromContext(const std::filesystem::path& contextDir, const char* imageTag)
    {
        LPCSTR tag = imageTag;
        WSLCBuildImageOptions options{
            .Tags = {&tag, 1},
        };
        return BuildImageFromContext(contextDir, &options);
    }

    struct BlockingOperation
    {
        NON_COPYABLE(BlockingOperation);
        NON_MOVABLE(BlockingOperation);

        BlockingOperation(std::function<HRESULT(HANDLE)>&& Operation, HRESULT ExpectedResult = S_OK, bool AllowEarlyCompletion = false, bool UseOverlappedWritePipe = false) :
            m_operation(std::move(Operation)), m_expectedResult(ExpectedResult), m_allowEarlyCompletion(AllowEarlyCompletion)
        {
            auto [pipeRead, pipeWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(100000, false, UseOverlappedWritePipe);

            m_operationThread = std::thread(&BlockingOperation::RunOperation, this, std::move(pipeWrite));
            m_ioThread = std::thread(&BlockingOperation::RunIO, this, std::move(pipeRead));

            // Wait for the operation to be running before continuing.
            VERIFY_IS_TRUE(m_startedEvent.wait(60 * 1000));
        }

        ~BlockingOperation()
        {
            if (m_operationThread.joinable())
            {
                m_operationThread.join();
            }

            if (m_ioThread.joinable())
            {
                m_ioThread.join();
            }
        }

        void RunOperation(wil::unique_hfile Handle)
        {
            m_result.set_value(m_operation(Handle.get()));

            // Fail if the operation completed before the test signaled completion
            // (unless early completion is expected, e.g. session termination).
            // Don't use VERIFY macros since this is running in a separate thread.
            WI_ASSERT(m_allowEarlyCompletion || m_testCompleteEvent.is_signaled());
        }

        void RunIO(wil::unique_hfile Handle)
        {
            std::vector<char> buffer(1024 * 1024);
            while (true)
            {
                DWORD bytesRead{};
                if (!ReadFile(Handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
                {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                    {
                        LogError("Unexpected ReadFile() error: %u", GetLastError());
                    }

                    break;
                }

                if (bytesRead == 0)
                {
                    break;
                }

                if (!m_startedEvent.is_signaled())
                {
                    m_startedEvent.SetEvent();
                }

                // Block until the test completes.
                if (!m_testCompleteEvent.wait(60 * 1000))
                {
                    LogError("Timed out waiting for test completion");
                    break;
                }
            }
        }

        void Complete()
        {
            m_testCompleteEvent.SetEvent();

            VERIFY_ARE_EQUAL(m_expectedResult, m_result.get_future().get());
        }

        std::function<HRESULT(HANDLE)> m_operation;
        wil::unique_event m_startedEvent{wil::EventOptions::ManualReset};
        wil::unique_event m_testCompleteEvent{wil::EventOptions::ManualReset};
        std::thread m_operationThread;
        std::thread m_ioThread;
        std::promise<HRESULT> m_result;
        HRESULT m_expectedResult{};
        bool m_allowEarlyCompletion{};
    };

    static void ValidateHandleOutput(HANDLE handle, const std::string& expectedOutput)
    {
        VERIFY_ARE_EQUAL(EscapeString(expectedOutput), EscapeString(ReadToString(handle)));
    }
};
