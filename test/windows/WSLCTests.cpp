/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCTests.cpp

Abstract:

    This file contains test cases for the WSLC API.

--*/

#include "precomp.h"
#include "Common.h"
#include "wslc.h"
#include "wslccompat.h"
#include "WSLCProcessLauncher.h"
#include "WSLCContainerLauncher.h"
#include "WslCoreFilesystem.h"
#include "hcs.hpp"
#include "ContainerNameGenerator.h"
#include "wslc/e2e/WSLCE2EHelpers.h"
#include "HttpHeaderEndDetector.h"
#include <nlohmann/json.hpp>

using namespace std::literals::chrono_literals;
using namespace wsl::windows::common::registry;
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

class WSLCTests
{
    WSLC_TEST_CLASS(WSLCTests)

    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WSLCSessionSettings m_defaultSessionSettings{};
    wil::com_ptr<IWSLCSession> m_defaultSession;
    static inline auto c_testSessionName = L"wslc-test";

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // The WSLC SDK tests use this same storage to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";
        m_defaultSessionSettings = GetDefaultSessionSettings(c_testSessionName, true, WSLCNetworkingModeConsomme);
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

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        m_defaultSession.reset();

        // Keep the VHD when running in -f mode, to speed up subsequent test runs.
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
        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> Containers;
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

    std::string PushImageToRegistry(const std::string& imageName, const std::string& registryAddress, const std::string& registryAuth)
    {
        auto [repo, tag] = ParseImage(imageName);
        auto registryImage = std::format("{}/{}:{}", registryAddress, repo, tag.value_or("latest"));
        auto registryRepo = std::format("{}/{}", registryAddress, repo);
        auto registryTag = tag.value_or("latest");

        WSLCTagImageOptions tagOptions{};
        tagOptions.Image = imageName.c_str();
        tagOptions.Repo = registryRepo.c_str();
        tagOptions.Tag = registryTag.c_str();

        // Tag the image with the registry address so it can be pushed.
        VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));

        // Ensures the tag is removed to allow tests to try to push or pull the same image again.
        auto cleanup = wil::scope_exit_log(
            WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_FAILED(DeleteImageNoThrow(registryImage, WSLCDeleteImageFlagsNone).first); });

        VERIFY_SUCCEEDED(m_defaultSession->PushImage(registryImage.c_str(), registryAuth.c_str(), nullptr, nullptr));

        return registryImage;
    }

    WSLC_TEST_METHOD(GetVersion)
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        WSLCVersion version{};

        VERIFY_SUCCEEDED(sessionManager->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
    }

    WSLC_TEST_METHOD(IsClientVersionSupported)
    {
        wil::com_ptr<IWSLCCompatSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        BOOL isSupported = FALSE;

        // The current version should always be supported.
        const WSLCCompatVersion currentVersion{WSL_PACKAGE_VERSION_MAJOR, WSL_PACKAGE_VERSION_MINOR, WSL_PACKAGE_VERSION_REVISION};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&currentVersion, &isSupported));
        VERIFY_IS_TRUE(isSupported);

        // A very old version should not be supported.
        const WSLCCompatVersion oldVersion{1, 0, 0};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&oldVersion, &isSupported));
        VERIFY_IS_FALSE(isSupported);

        // A very high version should be supported.
        const WSLCCompatVersion futureVersion{99, 0, 0};
        VERIFY_SUCCEEDED(sessionManager->IsClientVersionSupported(&futureVersion, &isSupported));
        VERIFY_IS_TRUE(isSupported);
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

    WSLC_TEST_METHOD(ListSessionsReturnsSessionWithDisplayName)
    {
        auto sessionManager = OpenSessionManager();

        // Act: list sessions
        {
            wil::unique_cotaskmem_array_ptr<WSLCSessionListEntry> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            // Assert
            VERIFY_ARE_EQUAL(sessions.size(), 1u);
            const auto& info = sessions[0];

            // SessionId is implementation detail (starts at 1), so we only assert DisplayName here.
            VERIFY_ARE_EQUAL(std::wstring(info.DisplayName), c_testSessionName);
        }

        // List multiple sessions.
        {
            auto session2 = CreateSession(GetDefaultSessionSettings(L"wslc-test-list-2"));

            wil::unique_cotaskmem_array_ptr<WSLCSessionListEntry> sessions;
            VERIFY_SUCCEEDED(sessionManager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            VERIFY_ARE_EQUAL(sessions.size(), 2);

            std::vector<std::wstring> displayNames;
            for (const auto& e : sessions)
            {
                displayNames.push_back(e.DisplayName);
            }

            std::ranges::sort(displayNames);

            VERIFY_ARE_EQUAL(displayNames[0], c_testSessionName);
            VERIFY_ARE_EQUAL(displayNames[1], L"wslc-test-list-2");
        }
    }

    WSLC_TEST_METHOD(OpenSessionByNameFindsExistingSession)
    {
        auto sessionManager = OpenSessionManager();

        // Act: open by the same display name
        wil::com_ptr<IWSLCSession> opened;
        VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(c_testSessionName, &opened));
        VERIFY_IS_NOT_NULL(opened.get());

        // And verify we get WSLC_E_SESSION_NOT_FOUND for a nonexistent name
        wil::com_ptr<IWSLCSession> notFound;
        auto hr = sessionManager->OpenSessionByName(L"this-name-does-not-exist", &notFound);
        VERIFY_ARE_EQUAL(hr, WSLC_E_SESSION_NOT_FOUND);
    }

    WSLC_TEST_METHOD(GetDisplayNameReturnsSessionName)
    {
        wil::unique_cotaskmem_string displayName;
        VERIFY_SUCCEEDED(m_defaultSession->GetDisplayName(&displayName));
        VERIFY_IS_NOT_NULL(displayName.get());
        VERIFY_ARE_EQUAL(std::wstring(displayName.get()), c_testSessionName);
    }

    WSLC_TEST_METHOD(CreateSessionValidation)
    {
        auto sessionManager = OpenSessionManager();

        // Reject NULL DisplayName.
        {
            auto settings = GetDefaultSessionSettings(nullptr);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Reject DisplayName at exact boundary (no room for null terminator).
        {
            std::wstring boundaryName(std::size(WSLCSessionListEntry{}.DisplayName), L'x');
            auto settings = GetDefaultSessionSettings(boundaryName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Reject too long DisplayName.
        {
            std::wstring longName(std::size(WSLCSessionListEntry{}.DisplayName) + 1, L'x');
            auto settings = GetDefaultSessionSettings(longName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), WSLC_E_INVALID_SESSION_NAME);
        }

        // Validate that creating a session on a non-existing storage fails if WSLCSessionStorageFlagsNoCreate is set.
        {
            auto settings = GetDefaultSessionSettings(L"storage-not-found");
            settings.StoragePath = L"C:\\does-not-exist";
            settings.StorageFlags = WSLCSessionStorageFlagsNoCreate;
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        }

        // Reject invalid storage flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-storage-flags");
            settings.StorageFlags = static_cast<WSLCSessionStorageFlags>(0x4);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
        }

        // Reject non-empty storage directory that doesn't contain a session VHD.
        {
            const auto storagePath = std::filesystem::temp_directory_path() /
                                     std::format(L"wslc-test-storage-{}-{}", GetCurrentProcessId(), GetTickCount64());
            std::filesystem::create_directories(storagePath);
            auto cleanup = wil::scope_exit([&]() {
                std::error_code ignored;
                std::filesystem::remove_all(storagePath, ignored);
            });

            std::ofstream{storagePath / L"userfile.txt"} << "data";

            auto settings = GetDefaultSessionSettings(L"storage-not-empty");
            const auto storagePathString = storagePath.wstring();
            settings.StoragePath = storagePathString.c_str();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
            ValidateCOMErrorMessage(std::format(L"Cannot use '{}' as session storage because the directory is not empty", storagePathString));
        }

        // Reject storage path that exists but is not a directory.
        {
            const auto storagePath = std::filesystem::temp_directory_path() /
                                     std::format(L"wslc-test-storage-file-{}-{}", GetCurrentProcessId(), GetTickCount64());
            std::ofstream{storagePath} << "data";
            auto cleanup = wil::scope_exit([&]() {
                std::error_code ignored;
                std::filesystem::remove(storagePath, ignored);
            });

            auto settings = GetDefaultSessionSettings(L"storage-not-directory");
            const auto storagePathString = storagePath.wstring();
            settings.StoragePath = storagePathString.c_str();
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session), E_INVALIDARG);
            ValidateCOMErrorMessage(std::format(L"Cannot use '{}' as session storage because it is not a directory", storagePathString));
        }

        // Reject invalid session flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-session-flags");
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(E_INVALIDARG, sessionManager->CreateSession(&settings, static_cast<WSLCSessionFlags>(0x4), nullptr, &session));
        }

        // Reject invalid feature flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-feature-flags");
            settings.FeatureFlags = static_cast<WSLCFeatureFlags>(0x40);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(E_INVALIDARG, sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session));
        }

        // Reject NULL output pointers across the session manager API.
        {
            auto settings = GetDefaultSessionSettings(L"null-out-session");
            VERIFY_ARE_EQUAL(
                HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->OpenSession(0, nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->OpenSessionByName(c_testSessionName, nullptr));

            WSLCSessionListEntry* entries = nullptr;
            ULONG count = 0;
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->ListSessions(nullptr, &count));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), sessionManager->ListSessions(&entries, nullptr));
        }

        // The session object must reject NULL output pointers.
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetId(nullptr));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetDisplayName(nullptr));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->GetState(nullptr));
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

    WSLC_TEST_METHOD(VmOwnerMatchesSessionDisplayName)
    {
        // The default session (c_testSessionName) is already running from class setup.
        // Verify its display name appears as a VM owner in hcsdiag output.
        auto vms = ListVms();

        auto found = std::ranges::find_if(vms, [](const auto& vm) { return vm.Owner == c_testSessionName; });
        if (found == vms.end())
        {
            LogError("Expected VM owner '%ws' not found. Owners:", c_testSessionName);
            for (const auto& vm : vms)
            {
                LogError("  '%ws'", vm.Owner.c_str());
            }

            VERIFY_FAIL();
        }
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

    std::set<std::string> ListVolumes(const std::vector<WSLCFilter>& Filters = {})
    {
        const WSLCFilter* filtersPtr = Filters.empty() ? nullptr : Filters.data();
        const ULONG filtersCount = static_cast<ULONG>(Filters.size());

        wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> volumes;
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(filtersPtr, filtersCount, volumes.addressof(), volumes.size_address<ULONG>()));

        std::set<std::string> names;
        for (const auto& v : volumes)
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

    WSLC_TEST_METHOD(PullImage)
    {
        {
            // Start a local registry without auth and push hello-world:latest to it.
            auto [registryContainer, registryAddress] = StartLocalRegistry(*m_defaultSession);

            auto image = PushImageToRegistry("hello-world:latest", registryAddress, BuildRegistryAuthHeader("", ""));
            ExpectImagePresent(*m_defaultSession, image.c_str(), false);

            VERIFY_SUCCEEDED(m_defaultSession->PullImage(image.c_str(), nullptr, nullptr, nullptr));
            auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(DeleteImageNoThrow(image, WSLCDeleteImageFlagsForce).first); });

            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, image.c_str());
            WSLCContainerLauncher launcher(image, "wslc-pull-image-container");

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        {
            std::wstring expectedError =
                L"pull access denied for does-not, repository does not exist or may require 'docker login': denied: requested "
                L"access to the resource is denied";

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("does-not:exist", nullptr, nullptr, nullptr), WSLC_E_IMAGE_NOT_FOUND);
            ValidateCOMErrorMessage(expectedError.c_str());
        }

        // Validate that PullImage() returns the appropriate error if the session is terminated.
        {
            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto cleanup = wil::scope_exit([&]() {
                ResetTestSession(); // Reopen the test session since the session was terminated.
            });

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("hello-world:linux", nullptr, nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    WSLC_TEST_METHOD(PullImageAdvanced)
    {
        // Start a local registry without auth to avoid Docker Hub rate limits.
        auto [registryContainer, registryAddress] = StartLocalRegistry(*m_defaultSession);
        auto auth = BuildRegistryAuthHeader("", "");

        auto validatePull = [&](const std::string& sourceImage) {
            // Push the source image to the local registry.
            auto registryImage = PushImageToRegistry(sourceImage, registryAddress, auth);
            ExpectImagePresent(*m_defaultSession, registryImage.c_str(), false);

            VERIFY_SUCCEEDED(m_defaultSession->PullImage(registryImage.c_str(), nullptr, nullptr, nullptr));

            auto cleanup =
                wil::scope_exit([&]() { LOG_IF_FAILED(DeleteImageNoThrow(registryImage, WSLCDeleteImageFlagsForce).first); });

            ExpectImagePresent(*m_defaultSession, registryImage.c_str());
        };

        validatePull("debian:latest");
        validatePull("alpine:latest");
        validatePull("hello-world:latest");
    }

    WSLC_TEST_METHOD(PullImageFromDockerHub)
    {
        SKIP_TEST_UNSTABLE();

        auto validatePull = [&](const std::string& Image, const std::optional<std::string>& ExpectedTag = {}) {
            VERIFY_SUCCEEDED(m_defaultSession->PullImage(Image.c_str(), nullptr, nullptr, nullptr));

            auto cleanup = wil::scope_exit(
                [&]() { LOG_IF_FAILED(DeleteImageNoThrow(ExpectedTag.value_or(Image), WSLCDeleteImageFlagsForce).first); });

            if (!ExpectedTag.has_value())
            {
                wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
                VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

                for (const auto& e : images)
                {
                    wil::unique_cotaskmem_ansistring json;
                    VERIFY_SUCCEEDED(m_defaultSession->InspectImage(e.Hash, &json));

                    auto parsed = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectImage>(json.get());

                    for (const auto& repoTag : parsed.RepoDigests.value_or({}))
                    {
                        if (Image == repoTag)
                        {
                            return;
                        }
                    }
                }

                LogError("Expected digest '%hs' not found ", Image.c_str());

                VERIFY_FAIL();
            }
            else
            {
                ExpectImagePresent(*m_defaultSession, ExpectedTag->c_str());
            }
        };

        validatePull("ubuntu@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30", {});
        validatePull("ubuntu", "ubuntu:latest");
        validatePull("debian:bookworm", "debian:bookworm");
        validatePull("pytorch/pytorch", "pytorch/pytorch:latest");
        validatePull("registry.k8s.io/pause:3.2", "registry.k8s.io/pause:3.2");

        // Validate that PullImage() fails appropriately when the session runs out of space.
        {
            auto settings = GetDefaultSessionSettings(L"wslc-pull-image-out-of-space", false);
            settings.NetworkingMode = WSLCNetworkingModeConsomme;
            settings.MemoryMb = 1024;
            auto session = CreateSession(settings);

            VERIFY_ARE_EQUAL(session->PullImage("pytorch/pytorch", nullptr, nullptr, nullptr), E_FAIL);

            ValidateCOMErrorMessageContains(L"no space left on device");
        }
    }

    WSLC_TEST_METHOD(PushImage)
    {
        auto emptyAuth = BuildRegistryAuthHeader("", "");

        // Validate that pushing a non-existent image fails.
        {
            VERIFY_ARE_EQUAL(m_defaultSession->PushImage("does-not-exist:latest", emptyAuth.c_str(), nullptr, nullptr), E_FAIL);
            ValidateCOMErrorMessage(L"An image does not exist locally with the tag: does-not-exist");
        }

        // Validate passing empty auth string returns an appropriate error.
        {
            VERIFY_ARE_EQUAL(m_defaultSession->PushImage("does-not-exist:latest", "", nullptr, nullptr), E_INVALIDARG);
        }

        // Validate that PushImage() returns the appropriate error if the session is terminated.
        {
            VERIFY_SUCCEEDED(m_defaultSession->Terminate());
            auto cleanup = wil::scope_exit([&]() { ResetTestSession(); });

            VERIFY_ARE_EQUAL(m_defaultSession->PushImage("hello-world:latest", emptyAuth.c_str(), nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    WSLC_TEST_METHOD(Authenticate)
    {
        constexpr auto c_username = "wslctest";
        constexpr auto c_password = "password";

        auto [registryContainer, registryAddress] = StartLocalRegistry(*m_defaultSession, c_username, c_password);

        wil::unique_cotaskmem_ansistring token;
        VERIFY_ARE_EQUAL(m_defaultSession->Authenticate(registryAddress.c_str(), c_username, "wrong-password", &token), E_FAIL);
        ValidateCOMErrorMessageContains(L"failed with status: 401 Unauthorized");

        VERIFY_SUCCEEDED(m_defaultSession->Authenticate(registryAddress.c_str(), c_username, c_password, &token));
        VERIFY_IS_NOT_NULL(token.get());

        auto xRegistryAuth = BuildRegistryAuthHeader(c_username, c_password);
        auto image = PushImageToRegistry("hello-world:latest", registryAddress, xRegistryAuth);

        // Pulling without credentials should fail.
        VERIFY_ARE_EQUAL(m_defaultSession->PullImage(image.c_str(), nullptr, nullptr, nullptr), E_FAIL);
        ValidateCOMErrorMessageContains(L"no basic auth credentials");

        // Pulling with credentials should succeed.
        VERIFY_SUCCEEDED(m_defaultSession->PullImage(image.c_str(), xRegistryAuth.c_str(), nullptr, nullptr));
        ExpectImagePresent(*m_defaultSession, image.c_str());
    }

    WSLC_TEST_METHOD(ListImages)
    {
        // Setup: Ensure debian:latest is available
        ExpectImagePresent(*m_defaultSession, "debian:latest");

        // Create additional tags for testing
        WSLCTagImageOptions tagOptions{};
        tagOptions.Image = "debian:latest";
        tagOptions.Repo = "debian";
        tagOptions.Tag = "test-tag1";
        VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));
        tagOptions.Tag = "test-tag2";
        VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("debian:test-tag1", WSLCDeleteImageFlagsNone).first);
            LOG_IF_FAILED(DeleteImageNoThrow("debian:test-tag2", WSLCDeleteImageFlagsNone).first);
        });

        LogInfo("Test: Basic listing with nullptr options");
        {
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

            VERIFY_IS_TRUE(images.size() > 0);

            // Find debian images and verify they exist
            bool foundLatest = false, foundTag1 = false, foundTag2 = false;
            for (const auto& image : images)
            {
                std::string imageName = image.Image;
                if (imageName == "debian:latest")
                {
                    foundLatest = true;
                }
                if (imageName == "debian:test-tag1")
                {
                    foundTag1 = true;
                }
                if (imageName == "debian:test-tag2")
                {
                    foundTag2 = true;
                }
            }

            VERIFY_IS_TRUE(foundLatest);
            VERIFY_IS_TRUE(foundTag1);
            VERIFY_IS_TRUE(foundTag2);
        }

        LogInfo("Test: Verify all fields are populated");
        {
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

            std::string commonHash;
            int debianTagCount = 0;

            for (const auto& image : images)
            {
                std::string imageName = image.Image;
                if (imageName.starts_with("debian:"))
                {
                    debianTagCount++;

                    // Verify Hash field
                    VERIFY_IS_TRUE(strlen(image.Hash) > 0);
                    VERIFY_IS_TRUE(std::string(image.Hash).starts_with("sha256:"));

                    // All debian tags should have the same hash (same underlying image)
                    if (commonHash.empty())
                    {
                        commonHash = image.Hash;
                    }
                    else
                    {
                        VERIFY_ARE_EQUAL(commonHash, std::string(image.Hash));
                    }

                    // Verify Size field
                    VERIFY_IS_TRUE(image.Size > 0);

                    // Verify Created timestamp
                    VERIFY_IS_TRUE(image.Created > 0);
                }
            }

            VERIFY_IS_TRUE(debianTagCount >= 3); // At least debian:latest, test-tag1, test-tag2
        }

        LogInfo("Test: Multiple tags for same image return separate entries");
        {
            WSLCFilter filter{.Key = "reference", .Value = "debian"};
            WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = &filter, .FiltersCount = 1};

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));

            // Should find at least our 3 debian tags
            VERIFY_IS_TRUE(images.size() >= 3);

            // Verify each tag is a separate entry
            std::set<std::string> imageTags;
            for (const auto& image : images)
            {
                imageTags.insert(image.Image);
            }

            VERIFY_IS_TRUE(imageTags.contains("debian:latest"));
            VERIFY_IS_TRUE(imageTags.contains("debian:test-tag1"));
            VERIFY_IS_TRUE(imageTags.contains("debian:test-tag2"));
        }

        LogInfo("Test: Filter by specific reference");
        {
            WSLCFilter filter{.Key = "reference", .Value = "debian:test-tag1"};
            WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = &filter, .FiltersCount = 1};

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));

            // When filtering by exact tag, Docker returns all tags for that image
            // So we should get debian:latest, debian:test-tag1, debian:test-tag2
            bool foundTag1 = false;
            for (const auto& image : images)
            {
                std::string imageName = image.Image;
                if (imageName == "debian:test-tag1")
                {
                    foundTag1 = true;
                }
            }
            VERIFY_IS_TRUE(foundTag1);
        }

        LogInfo("Test: Digests flag");
        {
            WSLCFilter filter{.Key = "reference", .Value = "debian:latest"};
            WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsDigests, .Filters = &filter, .FiltersCount = 1};

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));

            // Check if digests are available (they may not be for all images)
            bool hasDigest = false;
            for (const auto& image : images)
            {
                if (strlen(image.Digest) > 0)
                {
                    hasDigest = true;
                    // Digest should be in format repo@sha256:...
                    VERIFY_IS_TRUE(std::string(image.Digest).find("@sha256:") != std::string::npos);
                }
            }
            // Note: Pulled images from registry should have digests, locally built may not
        }

        LogInfo("Test: Invalid flags are rejected");
        {
            constexpr auto c_invalidFlags = static_cast<WSLCListImagesFlags>(0x4 | 0x8);

            WSLCListImagesOptions options{.Flags = c_invalidFlags, .Filters = nullptr, .FiltersCount = 0};
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;

            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
        }

        LogInfo("Test: Before/Since filters");
        {
            // Get all images to find their IDs and creation times
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> allImages;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, allImages.addressof(), allImages.size_address<ULONG>()));

            std::string debianId, pythonId;
            LONGLONG debianCreated = 0, pythonCreated = 0;
            for (const auto& image : allImages)
            {
                std::string imageName = image.Image;
                if (imageName == "debian:latest")
                {
                    debianId = image.Hash;
                    debianCreated = image.Created;
                }
                else if (imageName == "python:3.12-alpine")
                {
                    pythonId = image.Hash;
                    pythonCreated = image.Created;
                }
            }

            VERIFY_IS_FALSE(debianId.empty());
            VERIFY_IS_FALSE(pythonId.empty());

            // Both Created timestamps must be populated and distinct so that the since/before
            // boundaries are unambiguous. Equal timestamps would make Docker's filter behavior
            // ambiguous and could reintroduce flakiness.
            VERIFY_IS_GREATER_THAN(debianCreated, 0LL);
            VERIFY_IS_GREATER_THAN(pythonCreated, 0LL);
            VERIFY_ARE_NOT_EQUAL(debianCreated, pythonCreated);

            // Determine which image is older/newer based on actual creation timestamps.
            // Image creation times come from the registry and can change independently.
            const bool debianIsOlder = debianCreated < pythonCreated;
            const auto& olderId = debianIsOlder ? debianId : pythonId;
            const auto& newerId = debianIsOlder ? pythonId : debianId;
            const auto* olderName = debianIsOlder ? "debian:latest" : "python:3.12-alpine";
            const auto* newerName = debianIsOlder ? "python:3.12-alpine" : "debian:latest";

            LogInfo(
                "Older image: %hs (Created: %lld), Newer image: %hs (Created: %lld)",
                olderName,
                debianIsOlder ? debianCreated : pythonCreated,
                newerName,
                debianIsOlder ? pythonCreated : debianCreated);

            // Test 'since' filter - images created after the older image
            {
                WSLCFilter filter{.Key = "since", .Value = olderId.c_str()};
                WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = &filter, .FiltersCount = 1};

                wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
                VERIFY_IS_TRUE(images.size() > 0);

                bool foundNewer = false;
                for (const auto& image : images)
                {
                    LogInfo("Image: %hs, Hash: %hs, Created: %lld", image.Image, image.Hash, image.Created);
                    if (std::string{image.Image} == newerName)
                    {
                        foundNewer = true;
                    }
                }

                VERIFY_IS_TRUE(foundNewer);
            }

            // Test 'before' filter - images created before the newer image
            {
                WSLCFilter filter{.Key = "before", .Value = newerId.c_str()};
                WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = &filter, .FiltersCount = 1};
                wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
                VERIFY_IS_TRUE(images.size() > 0);

                bool foundOlder = false;
                for (const auto& image : images)
                {
                    if (std::string{image.Image} == olderName)
                    {
                        foundOlder = true;
                    }
                }

                VERIFY_IS_TRUE(foundOlder);
            }
        }

        LogInfo("Test: Dangling filter");
        {
            // Setup a dangling image
            WSLCTagImageOptions tagOptions{};
            tagOptions.Image = "debian:latest";
            tagOptions.Repo = "alpine";
            tagOptions.Tag = "latest";
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));

            auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LoadTestImage(*m_defaultSession, "alpine:latest"); });

            // List only dangling images
            WSLCFilter danglingTrueFilter{.Key = "dangling", .Value = "true"};
            WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = &danglingTrueFilter, .FiltersCount = 1};

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> danglingImages;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, danglingImages.addressof(), danglingImages.size_address<ULONG>()));

            VERIFY_ARE_EQUAL(1, danglingImages.size());

            // All dangling images should have <none>:<none> as the tag
            for (const auto& image : danglingImages)
            {
                std::string imageName = image.Image;
                VERIFY_ARE_EQUAL(imageName, std::string("<none>:<none>"));
            }

            // List non-dangling images
            WSLCFilter danglingFalseFilter{.Key = "dangling", .Value = "false"};
            options.Filters = &danglingFalseFilter;
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> nonDanglingImages;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, nonDanglingImages.addressof(), nonDanglingImages.size_address<ULONG>()));
            VERIFY_IS_TRUE(nonDanglingImages.size() > 0);

            // None of these should be <none>:<none>
            for (const auto& image : nonDanglingImages)
            {
                std::string imageName = image.Image;
                VERIFY_ARE_NOT_EQUAL(imageName, std::string("<none>:<none>"));
            }
        }

        LogInfo("Test: Label filter");
        {
            // Test with no filters (nullptr)
            WSLCListImagesOptions options{.Flags = WSLCListImagesFlagsNone, .Filters = nullptr, .FiltersCount = 0};

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));

            // Test with single label filter
            {
                WSLCFilter filters[] = {{"label", "test.label"}};
                options.Filters = filters;
                options.FiltersCount = 1;

                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
            }

            // Test with multiple label filters (labels are AND'ed together)
            {
                WSLCFilter filters[] = {{"label", "test.label1"}, {"label", "test.label2=value"}};
                options.Filters = filters;
                options.FiltersCount = 2;

                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
            }

            // Note: To fully test label filtering with actual matches, would need to:
            // 1. Build an image with specific labels using docker build --label
            // 2. Filter with matching labels
            // 3. Verify the filtered image appears
            // This only tests the API usage not fail without requiring image builds
        }

        cleanup.reset();
        ExpectImagePresent(*m_defaultSession, "debian:test-tag1", false);
        ExpectImagePresent(*m_defaultSession, "debian:test-tag2", false);
        ExpectImagePresent(*m_defaultSession, "debian:latest", true);
    }

    WSLC_TEST_METHOD(LoadImage)
    {
        SKIP_TEST_SERVER();

        std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), fileSize.QuadPart, nullptr, nullptr));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "hello-world:latest");

        // Validate container launch from the loaded image
        {
            WSLCContainerLauncher launcher("hello-world:latest", "wslc-load-image-container");

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Validate that invalid tars fail with proper error message and code.
        {
            auto currentExecutableHandle = wil::open_file(wil::GetModuleFileNameW<std::wstring>().c_str());
            VERIFY_IS_TRUE(GetFileSizeEx(currentExecutableHandle.get(), &fileSize));

            VERIFY_ARE_EQUAL(
                m_defaultSession->LoadImage(ToCOMInputHandle(currentExecutableHandle.get()), fileSize.QuadPart, nullptr, nullptr), E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }

        // Validate that LoadImage fails when the input pipe is closed during reading.
        {
            wil::unique_handle pipeRead;
            wil::unique_handle pipeWrite;
            VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

            std::promise<HRESULT> loadResult;
            std::thread operationThread([&]() {
                loadResult.set_value(m_defaultSession->LoadImage(ToCOMInputHandle(pipeRead.get()), 1024 * 1024, nullptr, nullptr));
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to ensure the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            // Close the write end.
            pipeWrite.reset();

            VERIFY_ARE_EQUAL(E_FAIL, loadResult.get_future().get());
        }

        // Validate that LoadImage is aborted when the session terminates.
        {
            wil::unique_handle pipeRead;
            wil::unique_handle pipeWrite;
            VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

            std::promise<HRESULT> terminateResult;
            wil::unique_event testCompleted{wil::EventOptions::ManualReset};
            std::thread operationThread([&]() {
                terminateResult.set_value(m_defaultSession->LoadImage(ToCOMInputHandle(pipeRead.get()), 1024 * 1024, nullptr, nullptr));
                WI_ASSERT(testCompleted.is_signaled());
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to validate that the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            testCompleted.SetEvent();

            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto restore = ResetTestSession();

            auto hr = terminateResult.get_future().get();
            VERIFY_IS_TRUE(hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
        }
    }

    class CapturingImageLoadCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IImageLoadCallback, IFastRundown>
    {
    public:
        HRESULT OnImageLoaded(LPCSTR ImageName, EnumReferenceFormat Format) override
        {
            m_images.emplace_back(ImageName, Format);
            return S_OK;
        }

        const std::vector<std::pair<std::string, EnumReferenceFormat>>& GetImages() const
        {
            return m_images;
        }

    private:
        std::vector<std::pair<std::string, EnumReferenceFormat>> m_images;
    };

    WSLC_TEST_METHOD(LoadImageCallback)
    {
        SKIP_TEST_SERVER();

        const std::filesystem::path imageTar = L"LoadImageCallbackExport.tar";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });

        // Save both images into a single archive.
        {
            wil::unique_handle tarFile{
                CreateFileW(imageTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());

            std::vector<LPCSTR> names = {"debian:latest", "hello-world:latest"};
            WSLCStringArray array{.Values = names.data(), .Count = static_cast<ULONG>(names.size())};
            VERIFY_SUCCEEDED(m_defaultSession->SaveImages(ToCOMInputHandle(tarFile.get()), &array, nullptr, nullptr));
        }

        // Delete both images so that loading actually recreates them.
        DeleteImage("hello-world:latest", WSLCDeleteImageFlagsForce);
        DeleteImage("debian:latest", WSLCDeleteImageFlagsForce);
        ExpectImagePresent(*m_defaultSession, "hello-world:latest", false);
        ExpectImagePresent(*m_defaultSession, "debian:latest", false);

        auto callback = Microsoft::WRL::Make<CapturingImageLoadCallback>();
        {
            wil::unique_handle tarFile{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(tarFile.get(), &fileSize));
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(tarFile.get()), fileSize.QuadPart, nullptr, callback.Get()));
        }

        ExpectImagePresent(*m_defaultSession, "debian:latest");
        ExpectImagePresent(*m_defaultSession, "hello-world:latest");

        // Validate that both images have been reported.
        const auto loaded = callback->GetImages();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), loaded.size());
        VERIFY_IS_TRUE(std::ranges::find(loaded, std::make_pair(std::string("debian:latest"), EnumReferenceFormatTag)) != loaded.end());
        VERIFY_IS_TRUE(
            std::ranges::find(loaded, std::make_pair(std::string("hello-world:latest"), EnumReferenceFormatTag)) != loaded.end());
    }

    WSLC_TEST_METHOD(LoadImageCallbackById)
    {
        SKIP_TEST_SERVER();

        const std::filesystem::path imageTar = L"LoadImageCallbackByIdExport.tar";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });

        auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LoadTestImage(*m_defaultSession, "hello-world:latest"); });

        std::string imageId;
        {
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));
            for (const auto& image : images)
            {
                if (std::strcmp(image.Image, "hello-world:latest") == 0)
                {
                    imageId = image.Hash;
                    break;
                }
            }
        }

        VERIFY_IS_FALSE(imageId.empty());
        VERIFY_IS_TRUE(imageId.starts_with("sha256:"));

        // Save the image by ID.
        {
            wil::unique_handle tarFile{
                CreateFileW(imageTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());

            std::vector<LPCSTR> names = {imageId.c_str()};
            WSLCStringArray array{.Values = names.data(), .Count = static_cast<ULONG>(names.size())};
            VERIFY_SUCCEEDED(m_defaultSession->SaveImages(ToCOMInputHandle(tarFile.get()), &array, nullptr, nullptr));
        }

        auto callback = Microsoft::WRL::Make<CapturingImageLoadCallback>();
        {
            wil::unique_handle tarFile{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(tarFile.get(), &fileSize));
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(tarFile.get()), fileSize.QuadPart, nullptr, callback.Get()));
        }

        // Validate that the expected image ID was reported.
        const auto& loaded = callback->GetImages();
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), loaded.size());
        VERIFY_ARE_EQUAL(imageId, loaded[0].first);
        VERIFY_ARE_EQUAL(EnumReferenceFormatDigest, loaded[0].second);
    }

    WSLC_TEST_METHOD(ImportImage)
    {
        SKIP_TEST_SERVER();

        auto cleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(DeleteImageNoThrow("my-hello-world:test", WSLCDeleteImageFlagsNone).first); });

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        wil::unique_cotaskmem_ansistring imageId;
        VERIFY_SUCCEEDED(m_defaultSession->ImportImage(
            ToCOMInputHandle(imageTarFileHandle.get()), "my-hello-world:test", fileSize.QuadPart, nullptr, &imageId));

        ExpectImagePresent(*m_defaultSession, "my-hello-world:test");

        // Validate that containers can be started from the imported image.
        {
            WSLCContainerLauncher launcher("my-hello-world:test", "wslc-import-image-container", {"/hello"});

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Validate that ImportImage fails if no tag is passed
        {
            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(ToCOMInputHandle(imageTarFileHandle.get()), "my-hello-world", fileSize.QuadPart, nullptr, &imageId),
                E_INVALIDARG);
        }

        // Validate that invalid tars fail with proper error message and code.
        {
            auto currentExecutableHandle = wil::open_file(wil::GetModuleFileNameW<std::wstring>().c_str());

            VERIFY_IS_TRUE(GetFileSizeEx(currentExecutableHandle.get(), &fileSize));

            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(
                    ToCOMInputHandle(currentExecutableHandle.get()), "invalid-image:test", fileSize.QuadPart, nullptr, &imageId),
                E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }

        // Validate that a large (300MB) invalid tar fails with proper error message and code.
        {
            auto largeFile =
                wil::create_new_file(L"largefile", GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, FILE_FLAG_DELETE_ON_CLOSE);

            // Create an invalid header (docker ignores the entire file if its header is only null bytes).
            DWORD bytesWritten{};
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(largeFile.get(), "foo", 3, &bytesWritten, nullptr));
            THROW_LAST_ERROR_IF(SetFilePointer(largeFile.get(), static_cast<LONG>(300 * _1MB), nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER);

            THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(largeFile.get()));
            THROW_LAST_ERROR_IF(SetFilePointer(largeFile.get(), 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER);

            VERIFY_IS_TRUE(GetFileSizeEx(largeFile.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart, 300 * _1MB);

            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(ToCOMInputHandle(largeFile.get()), "invalid-large-image:test", fileSize.QuadPart, nullptr, &imageId),
                E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }

        // Validate that ImportImage fails when the input pipe is closed during reading.
        {
            wil::unique_handle pipeRead;
            wil::unique_handle pipeWrite;
            VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

            std::promise<HRESULT> importResult;
            std::thread operationThread([&]() {
                wil::unique_cotaskmem_ansistring id;
                importResult.set_value(
                    m_defaultSession->ImportImage(ToCOMInputHandle(pipeRead.get()), "broken-read:eof", 1024 * 1024, nullptr, &id));
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to ensure the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            // Close the write end.
            pipeWrite.reset();

            VERIFY_ARE_EQUAL(E_FAIL, importResult.get_future().get());
        }

        // Validate that ImportImage is aborted when the session terminates.
        {
            wil::unique_handle pipeRead;
            wil::unique_handle pipeWrite;
            VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

            std::promise<HRESULT> terminateResult;
            wil::unique_event testCompleted{wil::EventOptions::ManualReset};
            std::thread operationThread([&]() {
                wil::unique_cotaskmem_ansistring id;
                terminateResult.set_value(m_defaultSession->ImportImage(
                    ToCOMInputHandle(pipeRead.get()), "session-terminate:test", 1024 * 1024, nullptr, &id));
                WI_ASSERT(testCompleted.is_signaled());
            });

            auto threadCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operationThread.join(); });

            // Write some data to validate that the service has started reading from the pipe (pipe buffer is 2 bytes).
            DWORD bytesWritten{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(pipeWrite.get(), "data", 4, &bytesWritten, nullptr));

            testCompleted.SetEvent();

            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto restore = ResetTestSession();

            auto hr = terminateResult.get_future().get();
            VERIFY_IS_TRUE(hr == E_ABORT || hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
        }
    }

    WSLC_TEST_METHOD(DeleteImage)
    {
        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "alpine:latest");

        auto restore = wil::scope_exit([&]() { LoadTestImage(*m_defaultSession, "alpine:latest"); });

        // Launch a container to ensure that image deletion fails when in use.
        WSLCContainerLauncher launcher("alpine:latest", "test-delete-container-in-use", {"sleep", "99999"}, {}, "host");

        auto container = launcher.Launch(*m_defaultSession);

        // Verify that the container is in running state.
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        // Test delete failed if image in use.
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), DeleteImageNoThrow("alpine:latest", WSLCDeleteImageFlagsNone).first);

        // Force should succeed.
        auto deletedImages = DeleteImage("alpine:latest", WSLCDeleteImageFlagsForce);
        VERIFY_IS_TRUE(deletedImages.size() > 0);
        VERIFY_IS_TRUE(std::strlen(deletedImages[0].Image) > 0);

        // Verify that the image is no longer in the list of images.
        ExpectImagePresent(*m_defaultSession, "alpine:latest", false);

        // Test delete failed if image does not exist.
        VERIFY_ARE_EQUAL(WSLC_E_IMAGE_NOT_FOUND, DeleteImageNoThrow("alpine:latest", WSLCDeleteImageFlagsForce).first);

        // Validate that invalid flags are rejected.
        {
            WSLCDeleteImageOptions invalidOptions{.Image = "alpine:latest", .Flags = 0x4};
            VERIFY_ARE_EQUAL(
                m_defaultSession->DeleteImage(&invalidOptions, deletedImages.addressof(), deletedImages.size_address<ULONG>()), E_INVALIDARG);
        }
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
            .Flags = static_cast<WSLCBuildImageFlags>(0x8)};

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->BuildImage(&options, nullptr, nullptr));
    }

    WSLC_TEST_METHOD(AnonymousVolumes)
    {
        auto contextDir = std::filesystem::current_path() / "build-context";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);

            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-build:latest", WSLCDeleteImageFlagsForce).first);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "VOLUME /volume\n"; // Use VOLUME to force the creation of an anonymous volume.
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build:latest");

        const std::vector<WSLCFilter> anonymousVolumeFilters = {{"driver", "guest"}, {"label", "com.docker.volume.anonymous="}};

        // Session-restart scenario: an anonymous volume-backed container survives a session reset.
        {
            WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-anonymous-volume", {"test", "-d", "/volume"});
            auto container = launcher.Launch(*m_defaultSession);
            container.SetDeleteOnClose(false);

            auto containerId = container.Id();

            auto result = container.GetInitProcess();
            ValidateProcessOutput(result, {});

            ResetTestSession();

            // Manually cleanup the container and delete anonymous volumes since the session has been reset.
            auto containerCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                wil::com_ptr<IWSLCContainer> container;
                VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerId.c_str(), &container));

                VERIFY_SUCCEEDED(container->Delete(WSLCDeleteFlagsForce | WSLCDeleteFlagsDeleteVolumes));
            });

            // Validate that the session is correctly restarted.
            auto [containers, ports] = ListContainers(m_defaultSession.get());

            VERIFY_ARE_EQUAL(containers.size(), 1);
            VERIFY_ARE_EQUAL(containers[0].Id, containerId);
        }

        // Delete container without WSLCDeleteFlagsDeleteVolumes -> anonymous volume is leaked.
        {
            WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-delete-vol-leak", {"test", "-d", "/volume"});
            auto container = launcher.Launch(*m_defaultSession);
            container.GetInitProcess().Wait();
            container.SetDeleteOnClose(false);

            // Clean up any leaked anonymous volumes when this block exits.
            auto volumeCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
                ULONGLONG spaceReclaimed = 0;
                LOG_IF_FAILED(m_defaultSession->PruneVolumes(nullptr, 0, nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));
            });

            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 1u);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Anonymous volume was NOT deleted by Docker.
            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 1u);
        }

        // Delete container with WSLCDeleteFlagsDeleteVolumes -> anonymous volume is cleaned up.
        {
            WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-delete-vol-rm", {"sleep", "99999"});
            auto container = launcher.Launch(*m_defaultSession);
            container.SetDeleteOnClose(false);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 1u);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsDeleteVolumes));
            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 0u);
        }

        // Container with WSLCContainerFlagsRm -> anonymous volume cleaned up when the container auto-removes on exit.
        {
            WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-delete-vol-rm", {"sleep", "99999"});
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 1u);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            VERIFY_ARE_EQUAL(ListVolumes(anonymousVolumeFilters).size(), 0u);
        }
    }

    WSLC_TEST_METHOD(TagImage)
    {
        auto runTagImage = [&](LPCSTR Image, LPCSTR Repo, LPCSTR Tag) {
            WSLCTagImageOptions options{};
            options.Image = Image;
            options.Repo = Repo;
            options.Tag = Tag;

            return m_defaultSession->TagImage(&options);
        };

        // Positive test: Tag an existing image with a new tag in the same repository.
        {
            ExpectImagePresent(*m_defaultSession, "debian:latest");

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                DeleteImage("debian:test-tag", WSLCDeleteImageFlagsNoPrune);

                ExpectImagePresent(*m_defaultSession, "debian:test-tag", false);
                ExpectImagePresent(*m_defaultSession, "debian:latest");
            });

            VERIFY_SUCCEEDED(runTagImage("debian:latest", "debian", "test-tag"));

            // Verify both tags exist and point to the same image.
            ExpectImagePresent(*m_defaultSession, "debian:latest");
            ExpectImagePresent(*m_defaultSession, "debian:test-tag");

            // Verify they have the same image hash.
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

            std::string latestHash;
            std::string testTagHash;
            for (const auto& image : images)
            {
                if (std::strcmp(image.Image, "debian:latest") == 0)
                {
                    latestHash = image.Hash;
                }
                else if (std::strcmp(image.Image, "debian:test-tag") == 0)
                {
                    testTagHash = image.Hash;
                }
            }

            VERIFY_IS_FALSE(latestHash.empty());
            VERIFY_IS_FALSE(testTagHash.empty());
            VERIFY_ARE_EQUAL(latestHash, testTagHash);
        }

        // Positive test: Tag with a different repository name.
        {
            ExpectImagePresent(*m_defaultSession, "debian:latest");

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                DeleteImage("myrepo/myimage:v1.0.0", WSLCDeleteImageFlagsNoPrune);

                ExpectImagePresent(*m_defaultSession, "myrepo/myimage:v1.0.0", false);
            });

            VERIFY_SUCCEEDED(runTagImage("debian:latest", "myrepo/myimage", "v1.0.0"));

            ExpectImagePresent(*m_defaultSession, "myrepo/myimage:v1.0.0");
        }

        // Positive test: Tag using image ID.
        {
            ExpectImagePresent(*m_defaultSession, "debian:latest");

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                DeleteImage("debian:test-by-id", WSLCDeleteImageFlagsNoPrune);

                ExpectImagePresent(*m_defaultSession, "debian:test-by-id", false);
            });

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, images.addressof(), images.size_address<ULONG>()));

            std::string imageId;
            for (const auto& image : images)
            {
                if (std::strcmp(image.Image, "debian:latest") == 0)
                {
                    imageId = image.Hash;
                    break;
                }
            }
            VERIFY_IS_FALSE(imageId.empty());

            VERIFY_SUCCEEDED(runTagImage(imageId.c_str(), "debian", "test-by-id"));

            ExpectImagePresent(*m_defaultSession, "debian:test-by-id");
        }

        // Positive test: Overwrite existing tag.
        {
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                DeleteImage("test:duplicate-tag", WSLCDeleteImageFlagsNoPrune);

                ExpectImagePresent(*m_defaultSession, "test:duplicate-tag", false);
            });

            VERIFY_SUCCEEDED(runTagImage("debian:latest", "test", "duplicate-tag"));
            VERIFY_SUCCEEDED(runTagImage("debian:latest", "test", "duplicate-tag"));
        }

        // Negative test: Null options pointer.
        {
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), m_defaultSession->TagImage(nullptr));
        }

        // Negative test: Null Image field.
        {
            VERIFY_ARE_EQUAL(E_POINTER, runTagImage(nullptr, "test", "tag"));
        }

        // Negative test: Null Repo field.
        {
            VERIFY_ARE_EQUAL(E_POINTER, runTagImage("debian:latest", nullptr, "tag"));
        }

        // Negative test: Null Tag field.
        {
            VERIFY_ARE_EQUAL(E_POINTER, runTagImage("debian:latest", "test", nullptr));
        }

        // Negative test: Tag a non-existent image.
        {
            VERIFY_ARE_EQUAL(WSLC_E_IMAGE_NOT_FOUND, runTagImage("nonexistent:notfound", "test", "fail"));
            ValidateCOMErrorMessage(L"No such image: nonexistent:notfound");
        }

        // Negative test: Invalid tag format with spaces.
        {
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), runTagImage("debian:latest", "test", "invalid tag"));
            ValidateCOMErrorMessage(L"invalid tag format");
        }
    }

    WSLC_TEST_METHOD(InspectImage)
    {
        // Test inspect debian:latest
        {
            wil::unique_cotaskmem_ansistring output;
            VERIFY_SUCCEEDED(m_defaultSession->InspectImage("debian:latest", &output));

            // Verify output is valid JSON
            VERIFY_IS_NOT_NULL(output.get());
            VERIFY_IS_TRUE(std::strlen(output.get()) > 0);
            LogInfo("Inspect output: %hs", output.get());

            // Parse and validate JSON structure
            auto inspectResult = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectImage>(output.get());

            // Verify all fields exposed in wslc_schema::InspectImage
            VERIFY_IS_TRUE(inspectResult.Id.find("sha256:") == 0);

            VERIFY_IS_TRUE(inspectResult.RepoTags.has_value());
            VERIFY_IS_FALSE(inspectResult.RepoTags->empty());
            bool foundTag = false;
            for (const auto& tag : inspectResult.RepoTags.value())
            {
                if (tag.find("debian:latest") != std::string::npos)
                {
                    foundTag = true;
                    break;
                }
            }
            VERIFY_IS_TRUE(foundTag);

            // skip testing RepoDigests for loaded test image.
            VERIFY_IS_FALSE(inspectResult.Created.empty());
            VERIFY_IS_TRUE(inspectResult.Architecture == "amd64" || inspectResult.Architecture == "arm64");
            VERIFY_ARE_EQUAL("linux", inspectResult.Os);
            VERIFY_IS_TRUE(inspectResult.Size > 0);
            VERIFY_IS_TRUE(inspectResult.Metadata.has_value());
            VERIFY_IS_TRUE(inspectResult.Metadata->size() > 0);

            VERIFY_IS_TRUE(inspectResult.Config.has_value());
            const auto& config = inspectResult.Config.value();
            VERIFY_IS_TRUE(config.Cmd.has_value());
            VERIFY_IS_TRUE(config.Cmd->size() > 0);
            VERIFY_IS_TRUE(config.Entrypoint.has_value());
            VERIFY_ARE_EQUAL(0, config.Entrypoint->size());
            VERIFY_IS_TRUE(config.Env.has_value());
            VERIFY_IS_TRUE(config.Env->size() > 0);
            VERIFY_IS_FALSE(config.Labels.has_value());
        }

        // Negative test: Image not found
        {
            wil::unique_cotaskmem_ansistring output;
            VERIFY_ARE_EQUAL(WSLC_E_IMAGE_NOT_FOUND, m_defaultSession->InspectImage("nonexistent:image", &output));
            ValidateCOMErrorMessage(L"No such image: nonexistent:image");
        }

        // Negative test: Bad image name input
        {
            wil::unique_cotaskmem_ansistring output;

            std::string longImageName(WSLC_MAX_IMAGE_NAME_LENGTH + 1, 'a');
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->InspectImage(longImageName.c_str(), &output));

            // Invalid name.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), m_defaultSession->InspectImage("debian latest", &output));
            ValidateCOMErrorMessage(L"invalid reference format");

            // Attempt to fake to call search endpoint. Our implementation escaped the image name correctly.
            VERIFY_ARE_EQUAL(WSLC_E_IMAGE_NOT_FOUND, m_defaultSession->InspectImage("search/debian:latest", &output));
            ValidateCOMErrorMessage(L"No such image: search/debian:latest");
        }
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

    WSLC_TEST_METHOD(SaveImage)
    {
        {
            std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            // Load the image from a saved tar
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), fileSize.QuadPart, nullptr, nullptr));
            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            WSLCContainerLauncher launcher("hello-world:latest", "wslc-hello-world-container");
            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        {
            std::filesystem::path imageTar = L"HelloWorldExported.tar";
            auto cleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });
            // Save the image to a tar file.
            {
                wil::unique_handle imageTarFileHandle{CreateFileW(
                    imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
                VERIFY_SUCCEEDED(m_defaultSession->SaveImage(ToCOMInputHandle(imageTarFileHandle.get()), "hello-world:latest", nullptr, nullptr));
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, true);
            }

            // Load the saved image to verify it's valid.
            {
                wil::unique_handle imageTarFileHandle{CreateFileW(
                    imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                // Load the image from a saved tar
                VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), fileSize.QuadPart, nullptr, nullptr));
                // Verify that the image is in the list of images.
                ExpectImagePresent(*m_defaultSession, "hello-world:latest");
                WSLCContainerLauncher launcher("hello-world:latest", "wslc-hello-world-container");
                auto container = launcher.Launch(*m_defaultSession);
                auto result = container.GetInitProcess().WaitAndCaptureOutput();
                VERIFY_ARE_EQUAL(0, result.Code);
                VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
            }
        }

        // Try to save an invalid image.
        {
            std::filesystem::path imageTar = L"HelloWorldError.tar";
            auto cleanfile =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });
            wil::unique_handle imageTarFileHandle{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
            VERIFY_FAILED(m_defaultSession->SaveImage(ToCOMInputHandle(imageTarFileHandle.get()), "hello-wld:latest", nullptr, nullptr));
            ValidateCOMErrorMessage(L"reference does not exist");

            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(fileSize.QuadPart > 0, false);
        }

        // Validate that cancellation works.
        {
            wil::unique_event cancelEvent{wil::EventOptions::ManualReset};

            BlockingOperation operation(
                [&](HANDLE handle) {
                    return m_defaultSession->SaveImage(ToCOMInputHandle(handle), "debian:latest", nullptr, cancelEvent.get());
                },
                E_ABORT);

            cancelEvent.SetEvent();
            operation.Complete();
        }
    }

    WSLC_TEST_METHOD(SaveImages)
    {
        auto BuildStringArray = [](const std::vector<LPCSTR>& values) -> WSLCStringArray {
            return WSLCStringArray{.Values = values.empty() ? nullptr : values.data(), .Count = static_cast<ULONG>(values.size())};
        };

        // Save multiple images to a single tar, delete one, then load back and verify.
        {
            std::filesystem::path imageTar = L"MultiImageExport.tar";
            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                DeleteFileW(imageTar.c_str());

                wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
                ULONGLONG spaceReclaimed = 0;

                LOG_IF_FAILED(m_defaultSession->PruneImages(
                    nullptr, 0, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed));
            });

            {
                wil::unique_handle imageTarFileHandle{CreateFileW(
                    imageTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

                std::vector<LPCSTR> names = {"hello-world:latest", "alpine:latest"};
                WSLCStringArray array = BuildStringArray(names);
                VERIFY_SUCCEEDED(m_defaultSession->SaveImages(ToCOMInputHandle(imageTarFileHandle.get()), &array, nullptr, nullptr));

                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                VERIFY_IS_TRUE(fileSize.QuadPart > 0);
            }

            // Delete hello-world:latest and verify it's gone.
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deleted;
            WSLCDeleteImageOptions delOpts{};
            delOpts.Image = "hello-world:latest";
            delOpts.Flags = WSLCDeleteImageFlagsForce;
            VERIFY_SUCCEEDED(m_defaultSession->DeleteImage(&delOpts, &deleted, deleted.size_address<ULONG>()));
            ExpectImagePresent(*m_defaultSession, "hello-world:latest", false);

            // Load it back from the multi-image tar — hello-world should reappear and alpine should still be present.
            {
                wil::unique_handle imageTarFileHandle{CreateFileW(
                    imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
                VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
                LARGE_INTEGER fileSize{};
                VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
                VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), fileSize.QuadPart, nullptr, nullptr));
            }

            ExpectImagePresent(*m_defaultSession, "hello-world:latest");
            ExpectImagePresent(*m_defaultSession, "alpine:latest");

            // Sanity check that the loaded hello-world image is functional.
            WSLCContainerLauncher launcher("hello-world:latest", "wslc-multi-save-container");
            auto container = launcher.Launch(*m_defaultSession);

            auto output = container.GetInitProcess().WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(0, output.Code);
            VERIFY_IS_TRUE(output.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        // Single image via SaveImages — must produce a valid tar archive.
        {
            std::filesystem::path imageTar = L"MultiImageSingle.tar";
            auto cleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });

            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

            std::vector<LPCSTR> names = {"hello-world:latest"};
            WSLCStringArray array = BuildStringArray(names);
            VERIFY_SUCCEEDED(m_defaultSession->SaveImages(ToCOMInputHandle(imageTarFileHandle.get()), &array, nullptr, nullptr));

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_IS_TRUE(fileSize.QuadPart > 0);
        }

        // Validate that invalid input parameters are rejected.
        {
            // Use a real temp file so ToCOMInputHandle doesn't throw before SaveImages runs.
            std::filesystem::path placeholderTar = L"MultiImageValidation.tar";
            auto placeholderCleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(placeholderTar.c_str())); });

            wil::unique_handle placeholder{CreateFileW(
                placeholderTar.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == placeholder.get());
            HANDLE phHandle = placeholder.get();

            // Empty array (Count=0).
            WSLCStringArray emptyArray{.Values = nullptr, .Count = 0};
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->SaveImages(ToCOMInputHandle(phHandle), &emptyArray, nullptr, nullptr));

            // Empty string entry.
            LPCSTR emptyEntry[] = {""};
            WSLCStringArray emptyEntryArray{.Values = emptyEntry, .Count = 1};
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->SaveImages(ToCOMInputHandle(phHandle), &emptyEntryArray, nullptr, nullptr));

            // Name longer than WSLC_MAX_IMAGE_NAME_LENGTH.
            std::string longName(WSLC_MAX_IMAGE_NAME_LENGTH + 1, 'a');
            LPCSTR longEntry[] = {longName.c_str()};
            WSLCStringArray longEntryArray{.Values = longEntry, .Count = 1};
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->SaveImages(ToCOMInputHandle(phHandle), &longEntryArray, nullptr, nullptr));

            // Too many images.
            std::vector<LPCSTR> names(WSLC_MAX_SAVE_IMAGES_COUNT + 1, "foo");
            WSLCStringArray tooManyArray = BuildStringArray(names);
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->SaveImages(ToCOMInputHandle(phHandle), &tooManyArray, nullptr, nullptr));
        }

        // Try to save with one of the images not found — must fail
        {
            std::filesystem::path imageTar = L"MultiImageError.tar";
            auto cleanup =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(imageTar.c_str())); });

            wil::unique_handle imageTarFileHandle{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

            std::vector<LPCSTR> names = {"alpine:latest", "not-found"};
            WSLCStringArray array = BuildStringArray(names);
            VERIFY_FAILED(m_defaultSession->SaveImages(ToCOMInputHandle(imageTarFileHandle.get()), &array, nullptr, nullptr));

            ValidateCOMErrorMessage(L"No such image: not-found");
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            VERIFY_ARE_EQUAL(0ull, static_cast<ULONGLONG>(fileSize.QuadPart));
        }
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

        wil::unique_handle tty = process.GetStdHandle(WSLCFDTty);

        auto validateTtyOutput = [&](const std::string& expected) {
            std::string buffer(expected.size(), '\0');

            DWORD offset = 0;

            while (offset < buffer.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(tty.get(), buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &bytesRead, nullptr));

                offset += bytesRead;
            }

            buffer.resize(offset);
            VERIFY_ARE_EQUAL(buffer, expected);
        };

        auto writeTty = [&](const std::string& content) {
            VERIFY_IS_TRUE(WriteFile(tty.get(), content.data(), static_cast<DWORD>(content.size()), nullptr, nullptr));
        };

        // Expect the shell prompt to be displayed
        validateTtyOutput("\033[?2004hsh-5.2# ");
        writeTty("echo OK\n");
        validateTtyOutput("echo OK\r\n\033[?2004l\rOK");

        // Exit the shell
        writeTty("exit\n");

        VERIFY_IS_TRUE(process.GetExitEvent().wait(30 * 1000));
    }

    void ValidateNetworking(WSLCNetworkingMode mode, bool enableDnsTunneling = false)
    {
        // Reuse the default session if settings match (same networking mode and DNS tunneling setting).
        auto createNewSession = mode != m_defaultSessionSettings.NetworkingMode ||
                                enableDnsTunneling != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsDnsTunneling);

        auto settings = GetDefaultSessionSettings(L"networking-test", false, mode);
        WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsDnsTunneling, enableDnsTunneling);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);

        // Verify that /etc/resolv.conf is correctly configured.
        if (enableDnsTunneling)
        {
            auto result = ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver ", "/etc/resolv.conf"}, 0);

            if (mode == WSLCNetworkingModeConsomme)
            {
                // Consomme points resolv.conf at the eth0 gateway.
                ExpectCommandResult(
                    session.get(),
                    {"/bin/sh",
                     "-c",
                     "ns=$(awk '/^nameserver/ {print $2; exit}' /etc/resolv.conf); "
                     "gw=$(ip route show default | awk '{print $3; exit}'); "
                     "[ -n \"$ns\" ] && [ -n \"$gw\" ] && [ \"$ns\" = \"$gw\" ]"},
                    0);
            }
            else
            {
                VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
            }
        }

        // Verify DNS resolution.
        // Note: without DNS tunneling, NAT mode uses the ICS SharedAccess DNS proxy which only supports UDP.
        // TCP DNS queries (dig +tcp) will time out without tunneling.
        VerifyDigDnsResolution(session.get(), "getent ahosts bing.com");
        VerifyDnsQueries(session.get(), mode, enableDnsTunneling);
    }

    TEST_METHOD(NATNetworking)
    {
        ValidateNetworking(WSLCNetworkingModeNAT);
    }

    TEST_METHOD(NATNetworkingWithDnsTunneling)
    {
        WINDOWS_11_TEST_ONLY();
        ValidateNetworking(WSLCNetworkingModeNAT, true);
    }

    TEST_METHOD(ConsommeNetworking)
    {
        ValidateNetworking(WSLCNetworkingModeConsomme);
    }

    TEST_METHOD(ConsommeNetworkingWithDnsTunneling)
    {
        WINDOWS_11_TEST_ONLY();
        ValidateNetworking(WSLCNetworkingModeConsomme, true);
    }

    // DNS test helpers

    void VerifyDigDnsResolution(IWSLCSession* session, const std::string& digCommandLine)
    {
        auto result = ExpectCommandResult(session, {"/bin/sh", "-c", digCommandLine}, 0);
        VERIFY_IS_FALSE(result.Output[1].empty());
    }

    void VerifyDnsQueries(IWSLCSession* session, WSLCNetworkingMode mode, bool enableDnsTunneling)
    {
        // TCP DNS works except for NAT without tunneling (ICS SharedAccess DNS proxy is UDP-only).
        const bool includeTcp = (mode != WSLCNetworkingModeNAT) || enableDnsTunneling;

        // UDP queries for all record types
        VerifyDigDnsResolution(session, "dig +short +time=5 A bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 AAAA bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 MX bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 NS bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 -x 8.8.8.8");
        VerifyDigDnsResolution(session, "dig +short +time=5 SOA bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 TXT bing.com");
        VerifyDigDnsResolution(session, "dig +time=5 CNAME bing.com");
        VerifyDigDnsResolution(session, "dig +time=5 SRV bing.com");

        if (includeTcp)
        {
            // ANY - dig expects a large response so it queries directly over TCP
            VerifyDigDnsResolution(session, "dig +short +time=5 ANY bing.com");

            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 A bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 AAAA bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 MX bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 NS bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 -x 8.8.8.8");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 SOA bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 TXT bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +time=5 CNAME bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +time=5 SRV bing.com");
        }
    }

    void ValidatePortMapping(WSLCNetworkingMode networkingMode)
    {
        auto settings = GetDefaultSessionSettings(L"port-mapping-test");
        settings.NetworkingMode = networkingMode;

        // Reuse the default session if the networking mode matches.
        auto createNewSession = networkingMode != m_defaultSessionSettings.NetworkingMode;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Install socat in the VM.
        {
            constexpr auto c_mountPoint = "/testdata";
            auto mountSource = std::filesystem::absolute(g_testDataPath);

            VERIFY_SUCCEEDED(session->MountWindowsFolder(mountSource.c_str(), c_mountPoint, true));
            auto unmount =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_FAILED(session->UnmountWindowsFolder(c_mountPoint)); });

            const auto installCommand = std::format("tdnf install -y --disablerepo='*' --nogpgcheck {}/packages/*.rpm", c_mountPoint);
            auto installSocat = WSLCProcessLauncher("/bin/sh", {"/bin/sh", "-c", installCommand}).Launch(*session);
            ValidateProcessOutput(installSocat, {}, 0, 120 * 1000);
        }

        auto listen = [&](short port, const char* content, bool ipv6) {
            auto cmd = std::format("echo -n '{}' | /usr/bin/socat -dd TCP{}-LISTEN:{},reuseaddr -", content, ipv6 ? "6" : "", port);
            auto process = WSLCProcessLauncher("/bin/sh", {"/bin/sh", "-c", cmd}).Launch(*session);
            WaitForOutput(process.GetStdHandle(2), "listening on");

            return process;
        };

        auto connectAndRead = [&](short port, int family) -> std::string {
            SOCKADDR_INET addr{};
            addr.si_family = family;
            INETADDR_SETLOOPBACK((PSOCKADDR)&addr);
            SS_PORT(&addr) = htons(port);

            wil::unique_socket hostSocket{socket(family, SOCK_STREAM, IPPROTO_TCP)};
            THROW_LAST_ERROR_IF(!hostSocket);
            THROW_LAST_ERROR_IF(connect(hostSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);

            return ReadToString(hostSocket.get());
        };

        auto expectContent = [&](short port, int family, const char* expected) {
            auto content = connectAndRead(port, family);
            VERIFY_ARE_EQUAL(content, expected);
        };

        auto expectNotBound = [&](short port, int family) {
            auto result = wil::ResultFromException([&]() { connectAndRead(port, family); });

            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(WSAECONNREFUSED));
        };

        // Map port
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        // Validate that the same port can't be bound twice
        VERIFY_ARE_EQUAL(session->MapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(WSAEADDRINUSE));

        // Check simple case
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that same port mapping can be reused
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that the connection is immediately reset if the port is not bound on the linux side
        expectContent(1234, AF_INET, "");

        // Add a ipv6 binding
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1234, 80));

        // Validate that ipv6 bindings work as well.
        listen(80, "port80ipv6", true);
        expectContent(1234, AF_INET6, "port80ipv6");

        // Unmap the ipv4 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));

        // Verify that a proper error is returned if the mapping doesn't exist
        // TODO: update once virtionet error code is fixed.
        VERIFY_ARE_EQUAL(
            session->UnmapVmPort(AF_INET, 1234, 80), networkingMode == WSLCNetworkingModeNAT ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_INVALIDARG);

        // Unmap the v6 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1234, 80));

        // Map another port as v6 only
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1235, 81));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1235, 81));
        VERIFY_ARE_EQUAL(session->UnmapVmPort(AF_INET6, 1235, 81), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        auto process =
            WSLCProcessLauncher{"/usr/bin/socat", {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"}}
                .Launch(*session);

        WaitForOutput(process.GetStdHandle(2), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));

        // Validate the 63-port limit.
        // TODO: Remove the 63-port limit by switching the relay's AcceptThread from
        // WaitForMultipleObjects to IO completion ports or similar.
        constexpr int c_maxPorts = 63;
        for (int i = 0; i < c_maxPorts; i++)
        {
            VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i)));
        }

        if (networkingMode == WSLCNetworkingModeNAT)
        {
            // In NAT mode, the 64th port mapping should fail with ERROR_TOO_MANY_OPEN_FILES since the relay process uses a file handle for each mapping.
            VERIFY_ARE_EQUAL(
                session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)),
                HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES));
        }
        else
        {
            VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)));
            VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)));
        }

        for (int i = 0; i < c_maxPorts; i++)
        {
            VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i)));
        }
    }

    TEST_METHOD(PortMappingNat)
    {
        ValidatePortMapping(WSLCNetworkingModeNAT);
    }

    TEST_METHOD(PortMappingConsomme)
    {
        ValidatePortMapping(WSLCNetworkingModeConsomme);
    }

    WSLC_TEST_METHOD(StuckVmTermination)
    {
        // Create a 'stuck' process
        auto process = WSLCProcessLauncher{"/bin/cat", {"/bin/cat"}, {}, WSLCProcessFlagsStdin}.Launch(*m_defaultSession);

        // Stop the service
        StopWslService();

        ResetTestSession(); // Reopen the session since the service was stopped.
    }

    void ValidateWindowsMounts(bool enableVirtioFs)
    {
        auto settings = GetDefaultSessionSettings(L"windows-mount-tests");
        WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs, enableVirtioFs);

        // Reuse the default session if possible.
        auto createNewSession = enableVirtioFs != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        auto expectedMountOptions = [&](bool readOnly) -> std::string {
            if (enableVirtioFs)
            {
                return std::format("/win-path*virtiofs*{},relatime*", readOnly ? "ro" : "rw");
            }
            else
            {
                return std::format(
                    "/win-path*9p*{},relatime,aname=*,cache=0x5,access=client,msize=65536,trans=fd,rfd=*,wfd=*", readOnly ? "ro" : "rw");
            }
        };

        auto testFolder = std::filesystem::current_path() / "test-folder";
        std::filesystem::create_directories(testFolder);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        // Validate writeable mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(false));

            // Validate that mount can't be stacked on each other
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that folder is writeable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt && sync"}, 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Validate that folder is not writeable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate that a read-only share cannot be made writeable via mount -o remount,rw.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Attempt an in-place remount to read-write from the guest.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "mount -o remount,rw /win-path"}, 0);

            // Verify the folder is still not writeable.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate that the device host enforces read-only even if the guest tries to bypass mount options.
        if (enableVirtioFs)
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Capture the mount source and type, unmount, then remount without read-only.
            ExpectCommandResult(
                session.get(),
                {"/bin/sh",
                 "-c",
                 "src=$(findmnt -n -o SOURCE /win-path) && "
                 "fstype=$(findmnt -n -o FSTYPE /win-path) && "
                 "umount /win-path && "
                 "mount -t $fstype $src /win-path"},
                0);

            // Verify the folder is still not writeable.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate various error paths
        {
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"relative-path", "/win-path", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"C:\\does-not-exist", "/win-path", true), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "relative-mountpoint", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "", true), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/not-mounted"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/proc"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
        }
    }

    WSLC_TEST_METHOD(WindowsMounts)
    {
        ValidateWindowsMounts(false);
    }

    WSLC_TEST_METHOD(WindowsMountsVirtioFs)
    {
        ValidateWindowsMounts(true);
    }

    // Validates that VirtioFs shares are reused across mount/unmount cycles for the same Windows folder.
    WSLC_TEST_METHOD(WindowsMountsVirtioFsShareReuse)
    {
        auto settings = GetDefaultSessionSettings(L"virtiofs-share-reuse-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);

        auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        auto testFolder = std::filesystem::current_path() / "test-folder-share-reuse";
        std::filesystem::create_directories(testFolder);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        auto getMountSource = [&](const char* mountPoint) -> std::string {
            auto cmd = std::format("findmnt -n -o SOURCE {}", mountPoint);
            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", cmd}, 0);
            return result.Output[1];
        };

        // Mount, capture the source (share GUID), unmount, remount, verify same GUID is reused.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            auto firstSource = getMountSource("/win-path");
            VERIFY_IS_FALSE(firstSource.empty());

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
            ExpectMount(session.get(), "/win-path", {});

            // Remount the same folder - should reuse the same share GUID.
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            auto secondSource = getMountSource("/win-path");

            VERIFY_ARE_EQUAL(firstSource, secondSource);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
        }

        // Verify that changing the read-only flag produces a different share GUID.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false));
            auto rwSource = getMountSource("/win-path");

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));

            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            auto roSource = getMountSource("/win-path");

            VERIFY_ARE_NOT_EQUAL(rwSource, roSource);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
        }
    }

    // Validate that the correct error is returned when too many virtiofs shares are mounted.
    WSLC_TEST_METHOD(VirtiofsVolumesLimit)
    {
        constexpr size_t c_maxVirtioFsShares = wsl::shared::c_maxVirtioFsShares;

        auto settings = GetDefaultSessionSettings(L"virtiofs-share-limit-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);

        // Use a dedicated session so the share count starts at zero (no GPU libraries are mounted).
        auto session = CreateSession(settings);

        auto testRoot = std::filesystem::current_path() / "test-folder-share-limit";
        std::filesystem::create_directories(testRoot);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testRoot); });

        auto folderForIndex = [&](size_t index) {
            auto folder = testRoot / std::to_string(index);
            std::filesystem::create_directories(folder);
            return folder;
        };

        // Mount distinct Windows folders (each creates a new share) until the limit is reached.
        size_t mounted = 0;
        HRESULT lastResult = S_OK;
        for (size_t i = 0; i <= c_maxVirtioFsShares; ++i)
        {
            auto folder = folderForIndex(i);
            auto mountPoint = std::format("/vfs-limit-{}", i);

            lastResult = session->MountWindowsFolder(folder.c_str(), mountPoint.c_str(), false);
            if (FAILED(lastResult))
            {
                break;
            }

            mounted++;
        }

        VERIFY_ARE_EQUAL(mounted, c_maxVirtioFsShares);
        VERIFY_ARE_EQUAL(lastResult, E_OUTOFMEMORY);
        ValidateCOMErrorMessage(
            L"Too many volumes have been mounted (limit: 15). Restart the session to mount more volumes. This will be fixed in a "
            L"future release.");

        // Reusing an already-created share must still succeed.
        auto reusedFolder = folderForIndex(0);
        VERIFY_SUCCEEDED(session->MountWindowsFolder(reusedFolder.c_str(), "/vfs-limit-reuse", false));
        VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/vfs-limit-reuse"));
    }

    // This test case validates that no file descriptors are leaked to user processes.
    WSLC_TEST_METHOD(Fd)
    {
        auto result = ExpectCommandResult(
            m_defaultSession.get(), {"/bin/sh", "-c", "echo /proc/self/fd/* && (readlink -v /proc/self/fd/* || true)"}, 0);

        // Note: fd/0 is opened by readlink to read the actual content of /proc/self/fd.
        if (!PathMatchSpecA(result.Output[1].c_str(), "/proc/self/fd/0 /proc/self/fd/1 /proc/self/fd/2\nsocket:*\nsocket:*"))
        {
            LogInfo("Found additional fds: %hs", result.Output[1].c_str());
            VERIFY_FAIL();
        }
    }

    WSLC_TEST_METHOD(GPU)
    {
        // Validate that trying to mount the shares without GPU support enabled fails.
        {
            auto settings = GetDefaultSessionSettings(L"gpu-test-disabled");
            WI_ClearFlag(settings.FeatureFlags, WslcFeatureFlagsGPU);

            auto createNewSession = WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsGPU);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

            // Validate that the GPU device is not available.
            ExpectMount(session.get(), "/usr/lib/wsl/drivers", {});
            ExpectMount(session.get(), "/usr/lib/wsl/lib", {});
        }

        // Validate that the GPU device is available when enabled.
        {
            auto settings = GetDefaultSessionSettings(L"gpu-test");
            WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsGPU);

            auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsGPU);
            auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

            // Validate that the GPU device is available.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "test -c /dev/dxg"}, 0);

            ExpectMount(
                session.get(),
                "/usr/lib/wsl/drivers",
                "/usr/lib/wsl/drivers*9p*relatime,aname=*,cache=0x5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            ExpectMount(
                session.get(),
                "/usr/lib/wsl/lib",
                "/usr/lib/wsl/lib none*overlay ro,relatime,lowerdir=/usr/lib/wsl/lib/packaged*");

            // Validate that the mount points are not writeable.
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}).Code, 1L);
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}).Code, 1L);
        }
    }

    WSLC_TEST_METHOD(ContainerGpu)
    {

        // Validate that setting the GPU flag on a non-GPU session fails.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-gpu-fail");
            launcher.SetContainerFlags(WSLCContainerFlagsGpu);

            auto [hr, _] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }

        auto restore = ResetTestSession();

        auto settings = GetDefaultSessionSettings(L"container-gpu-test", true);
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsGPU);

        auto session = CreateSession(settings);

        // Validate that GPU resources are available inside a container when WSLCContainerFlagsGpu is set.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-gpu", {"sleep", "99999"});
            launcher.SetContainerFlags(WSLCContainerFlagsGpu);

            auto container = launcher.Launch(*session);

            auto expect = [&](const std::vector<std::string> command,
                              int exitCode,
                              const std::map<int, std::string>& expectedOutput = {},
                              const std::vector<std::string>& env = {}) {
                auto process = WSLCProcessLauncher({}, command, env).Launch(container.Get());
                ValidateProcessOutput(process, expectedOutput, exitCode);
            };

            // Validate that /dev/dxg is available as a character device with read/write permissions.
            expect({"/bin/sh", "-c", "test -c /dev/dxg && test -r /dev/dxg && test -w /dev/dxg"}, 0);

            // Validate that the GPU library directory is mounted and contains libraries.
            expect({"/bin/sh", "-c", "test -d /usr/lib/wsl/lib && ls /usr/lib/wsl/lib | grep -q ."}, 0);

            // Validate that the GPU drivers directory is mounted and accessible.
            expect({"/bin/sh", "-c", "test -d /usr/lib/wsl/drivers"}, 0);

            // Validate that the GPU mount points are read-only.
            expect({"/usr/bin/touch", "/usr/lib/wsl/lib/test"}, 1);
            expect({"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}, 1);

            // Validate that the dynamic linker is configured to resolve the WSL GPU libraries.
            expect({"/bin/sh", "-c", "cat /etc/ld.so.conf.d/ld.wsl.conf"}, 0, {{1, "/usr/lib/wsl/lib\n"}});
            expect({"/bin/sh", "-c", "ldconfig -p | grep -q ' => /usr/lib/wsl/lib/'"}, 0);

            std::vector<std::string> expectedBinaries;
            for (const auto& entry : std::filesystem::directory_iterator("C:\\Windows\\system32\\lxss\\lib"))
            {
                const auto fileName = entry.path().filename().wstring();
                if (entry.is_regular_file() && fileName.find(L".so") == std::wstring::npos)
                {
                    expectedBinaries.push_back(wsl::shared::string::WideToMultiByte(fileName));
                }
            }

            if (expectedBinaries.empty())
            {
                LogWarning("No executables found in C:\\Windows\\system32\\lxss\\lib. Skipping GPU executable bind mount test");
            }
            else
            {
                for (const auto& e : expectedBinaries)
                {
                    expect({"test", "-x", std::format("/usr/bin/{}", e)}, 0);
                }
            }
        }

        // Validate that containers without the GPU flag do not have GPU resources.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-no-gpu", {"/bin/sh", "-c", "test -c /dev/dxg"});
            auto container = launcher.Launch(*session);

            ValidateContainerOutput(container, {{1, ""}}, 1);
        }

        // Validate that the directories are readable by non-root users.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-container-gpu-nobody", {"/bin/ls", "/usr/lib/wsl/lib", "/usr/lib/wsl/drivers"});

            launcher.SetContainerFlags(WSLCContainerFlagsGpu);
            launcher.SetUser("nobody");

            auto container = launcher.Launch(*session);

            ValidateContainerOutput(container, {}, 0);
        }
    }

    WSLC_TEST_METHOD(Modules)
    {
        // Sanity check.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    WSLC_TEST_METHOD(CreateRootNamespaceProcess)
    {
        // Reject invalid process flags.
        {
            WSLCProcessOptions options{};
            options.Flags = static_cast<WSLCProcessFlags>(0x4);
            wil::com_ptr<IWSLCProcess> process;
            int err = 0;
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateRootNamespaceProcess("/bin/true", &options, 0, 0, &process, &err));
        }

        // Simple case
        {
            auto result = ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "echo OK"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "OK\n");
            VERIFY_ARE_EQUAL(result.Output[2], "");
        }

        // Stdout + stderr
        {

            auto result = ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "echo stdout && (echo stderr 1>& 2)"}, 0);
            VERIFY_ARE_EQUAL(result.Output[1], "stdout\n");
            VERIFY_ARE_EQUAL(result.Output[2], "stderr\n");
        }

        // Write a large stdin buffer and expect it back on stdout.
        {
            std::vector<char> largeBuffer;
            std::string pattern = "ExpectedBufferContent";

            for (size_t i = 0; i < 1024 * 1024; i++)
            {
                largeBuffer.insert(largeBuffer.end(), pattern.begin(), pattern.end());
            }

            WSLCProcessLauncher launcher("/bin/sh", {"/bin/sh", "-c", "cat && (echo completed 1>& 2)"}, {}, WSLCProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

            std::unique_ptr<OverlappedIOHandle> writeStdin(new WriteHandle(process.GetStdHandle(0), largeBuffer));
            std::vector<std::unique_ptr<OverlappedIOHandle>> extraHandles;
            extraHandles.emplace_back(std::move(writeStdin));

            auto result = process.WaitAndCaptureOutput(INFINITE, std::move(extraHandles));

            VERIFY_IS_TRUE(std::equal(largeBuffer.begin(), largeBuffer.end(), result.Output[1].begin(), result.Output[1].end()));
            VERIFY_ARE_EQUAL(result.Output[2], "completed\n");

            // Validate that a null out handle is rejected.

            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(WSLCFDStdout, nullptr), HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));

            // Validate that every IWSLCProcess output pointer is rejected when null.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), process.Get().GetExitEvent(nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), process.Get().GetPid(nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), process.Get().GetState(nullptr, nullptr));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), process.Get().GetFlags(nullptr));

            // GetFlags succeeds with a valid pointer and reports the launched flags.
            WSLCProcessFlags flags{};
            VERIFY_SUCCEEDED(process.Get().GetFlags(&flags));
            VERIFY_IS_TRUE(WI_IsFlagSet(flags, WSLCProcessFlagsStdin));
        }

        // Create a stuck process and kill it.
        {
            WSLCProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLCProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

            // Try to send invalid signal to the process
            VERIFY_ARE_EQUAL(process.Get().Signal(9999), E_FAIL);

            // Send SIGKILL(9) to the process.
            VERIFY_SUCCEEDED(process.Get().Signal(WSLCSignalSIGKILL));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, WSLCSignalSIGKILL + 128);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            // Validate that process can't be signalled after it exited.
            VERIFY_ARE_EQUAL(process.Get().Signal(WSLCSignalSIGKILL), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Validate that errno is correctly propagated
        {
            WSLCProcessLauncher launcher("doesnotexist", {});

            auto [hresult, process, error] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 2); // ENOENT
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLCProcessLauncher launcher("/", {});

            auto [hresult, process, error] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);
            VERIFY_ARE_EQUAL(error, 13); // EACCESS
            VERIFY_IS_FALSE(process.has_value());
        }

        {
            WSLCProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLCProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);
            auto stdoutHandle = process.GetStdHandle(1);

            COMOutputHandle dummyHandle;
            // Verify that the same handle can only be acquired once.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(WSLCFDStdout, &dummyHandle), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that trying to acquire a std handle that doesn't exist fails as expected.
            VERIFY_ARE_EQUAL(process.Get().GetStdHandle(static_cast<WSLCFD>(3), &dummyHandle), E_INVALIDARG);

            // Validate that the process object correctly handle requests after the VM has terminated.
            ResetTestSession();
            VERIFY_ARE_EQUAL(process.Get().Signal(WSLCSignalSIGKILL), HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE));
        }

        // Validate that empty arguments are correctly handled.
        {
            WSLCProcessLauncher launcher({"/usr/bin/echo"}, {"/usr/bin/echo", "foo", "", "bar"});

            auto process = launcher.Launch(*m_defaultSession);
            ValidateProcessOutput(process, {{1, "foo  bar\n"}}); // expect two spaces for the empty argument.
        }

        // Validate error paths
        {
            WSLCProcessLauncher launcher("/bin/bash", {"/bin/bash"});
            launcher.SetUser("nobody"); // Custom users are not supported for root namespace processes.

            auto [hresult, error, process] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        }
    }

    WSLC_TEST_METHOD(CrashDumpCollection)
    {
        int processId = 0;

        // Cache the existing crash dumps so we can check that a new one is created.
        auto crashDumpsDir = std::filesystem::temp_directory_path() / "wslc-crashes";
        std::set<std::filesystem::path> existingDumps;

        if (std::filesystem::exists(crashDumpsDir))
        {
            existingDumps = {std::filesystem::directory_iterator(crashDumpsDir), std::filesystem::directory_iterator{}};
        }

        // Create a stuck process and crash it.
        {
            WSLCProcessLauncher launcher("/bin/cat", {"/bin/cat"}, {}, WSLCProcessFlagsStdin);

            auto process = launcher.Launch(*m_defaultSession);

            // Get the process id. This is need to identify the crash dump file.
            VERIFY_SUCCEEDED(process.Get().GetPid(&processId));

            // Send SIGSEV(11) to crash the process.
            VERIFY_SUCCEEDED(process.Get().Signal(WSLCSignalSIGSEGV));

            auto result = process.WaitAndCaptureOutput();
            VERIFY_ARE_EQUAL(result.Code, 128 + WSLCSignalSIGSEGV);
            VERIFY_ARE_EQUAL(result.Output[1], "");
            VERIFY_ARE_EQUAL(result.Output[2], "");

            VERIFY_ARE_EQUAL(process.Get().Signal(WSLCSignalSIGKILL), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }

        // Dumps files are named with the format: wsl-crash-<sessionId>-<pid>-<processname>-<code>.dmp
        // Check if a new file was added in crashDumpsDir matching the pattern and not in existingDumps.
        std::string expectedPattern = std::format("wsl-crash-*-{}-_usr_bin_cat-11.dmp", processId);

        auto dumpFile = wsl::shared::retry::RetryWithTimeout<std::filesystem::path>(
            [crashDumpsDir, expectedPattern, existingDumps]() {
                for (const auto& entry : std::filesystem::directory_iterator(crashDumpsDir))
                {
                    const auto& filePath = entry.path();
                    if (existingDumps.find(filePath) == existingDumps.end() &&
                        PathMatchSpecA(filePath.filename().string().c_str(), expectedPattern.c_str()))
                    {
                        return filePath;
                    }
                }

                throw wil::ResultException(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            },
            std::chrono::milliseconds{100},
            std::chrono::seconds{10});

        // Ensure that the dump file is cleaned up after test completion.
        auto cleanup = wil::scope_exit([&] {
            if (std::filesystem::exists(dumpFile))
            {
                std::filesystem::remove(dumpFile);
            }
        });

        VERIFY_IS_TRUE(std::filesystem::exists(dumpFile));
        VERIFY_IS_TRUE(std::filesystem::file_size(dumpFile) > 0);
    }

    WSLC_TEST_METHOD(VhdFormatting)
    {
        constexpr auto formatedVhd = L"test-format-vhd.vhdx";

        // TODO: Replace this by a proper SDK method once it exists
        auto tokenInfo = wil::get_token_information<TOKEN_USER>();
        wsl::core::filesystem::CreateVhd(formatedVhd, 100 * 1024 * 1024, tokenInfo->User.Sid, false, false);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(formatedVhd)); });

        // Format the disk.
        auto absoluteVhdPath = std::filesystem::absolute(formatedVhd).wstring();
        VERIFY_SUCCEEDED(m_defaultSession->FormatVirtualDisk(absoluteVhdPath.c_str()));

        // Validate error paths.
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"DoesNotExist.vhdx"), E_INVALIDARG);
        VERIFY_ARE_EQUAL(m_defaultSession->FormatVirtualDisk(L"C:\\DoesNotExist.vhdx"), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }

    // Exercises behavior that all volume drivers must implement identically:
    // create, duplicate-name rejection, multi-mount, cross-container read/write,
    // in-use deletion rejection, and clean deletion after the referencing container is removed.
    void ValidateNamedVolumeContract(std::string_view driver, const WSLCDriverOption* driverOpts, ULONG driverOptsCount)
    {
        const std::string driverStr(driver);
        const std::string volumeName = std::format("wslc-test-named-volume-{}", driver);

        // Best-effort cleanup in case of leftovers from a previous failed run.
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = driverStr.c_str();
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = driverOptsCount;

        // Create volume and validate duplicate volume name handling.
        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        VERIFY_ARE_EQUAL(std::string(volInfo.Name), volumeName);
        VERIFY_ARE_EQUAL(std::string(volInfo.Driver), driverStr);
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&volumeOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Verify the same named volume can be mounted more than once with different container paths.
        {
            WSLCContainerLauncher duplicateNamedVolumes(
                "debian:latest",
                std::format("named-volume-dup-{}", driver),
                {"/bin/sh", "-c", "echo duplicated >/data-a/dup.txt ; cat /data-b/dup.txt"});
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-a", false);
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-b", true);

            auto duplicateNamedVolumesContainer = duplicateNamedVolumes.Launch(*m_defaultSession);
            auto duplicateNamedVolumesProcess = duplicateNamedVolumesContainer.GetInitProcess();
            ValidateProcessOutput(duplicateNamedVolumesProcess, {{1, "duplicated\n"}});
        }

        // Verify CreateContainer with named volume mounts the volume into the container.
        {
            WSLCContainerLauncher writer(
                "debian:latest",
                std::format("named-volume-writer-{}", driver),
                {"/bin/sh", "-c", "echo wslc-named-volume >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});

            WSLCContainerLauncher reader(
                "debian:latest", std::format("named-volume-reader-{}", driver), {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "wslc-named-volume\n"}});
        }

        // Verify we cannot delete a named volume while a container references it.
        WSLCContainerLauncher holder("debian:latest", std::format("named-volume-holder-{}", driver), {"sleep", "99999"});
        holder.AddNamedVolume(volumeName, "/data", false);

        auto [holderCreateResult, holderContainerResult] = holder.CreateNoThrow(*m_defaultSession);
        VERIFY_SUCCEEDED(holderCreateResult);
        VERIFY_IS_TRUE(holderContainerResult.has_value());

        auto holderContainer = std::move(holderContainerResult.value());
        holderContainer.SetDeleteOnClose(false);

        VERIFY_ARE_EQUAL(m_defaultSession->DeleteVolume(volumeName.c_str()), HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));

        // Verify that after deleting the container, the volume can be deleted.
        VERIFY_SUCCEEDED(holderContainer.Get().Delete(WSLCDeleteFlagsNone));
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        cleanup.release();
    }

    WSLC_TEST_METHOD(NamedVolumesVhd)
    {
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        ValidateNamedVolumeContract("vhd", driverOpts, ARRAYSIZE(driverOpts));

        // VHD-driver-specific: validate the host-side .vhdx artifact and the
        // /mnt/wslc-volumes ext4 mount inside the VM appear and disappear with
        // the volume.
        const std::string volumeName = "wslc-test-named-volume-vhd-host";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));
        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::optional<std::string>{"*ext4*"});

        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        cleanup.release();

        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::nullopt);
        VERIFY_IS_FALSE(std::filesystem::exists(volumeVhdPath));
    }

    WSLC_TEST_METHOD(NamedVolumesVhdSeedsImageData)
    {
        // A freshly formatted VHD volume must be seeded with the image's content
        // on first use, just like a guest volume. mkfs.ext4 creates a lost+found
        // directory at the volume root; if it isn't removed, Docker treats the
        // volume as non-empty and skips the copy-up that seeds image data.
        // Mounting the empty volume over a directory the image is guaranteed to
        // populate (/etc) exercises that copy-up.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        const std::string volumeName = "wslc-test-named-volume-vhd-seed";

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "wslc-vhd-seed-container", {"/bin/sh", "-c", "ls -A /etc"});
        launcher.AddNamedVolume(volumeName, "/etc", false);

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);

        // Image content was seeded into the volume...
        VERIFY_IS_TRUE(
            result.Output[1].find("passwd") != std::string::npos,
            L"Image's /etc content should be seeded into the fresh VHD volume");

        // ...and the ext4 lost+found is gone, so it never blocked copy-up.
        VERIFY_IS_TRUE(
            result.Output[1].find("lost+found") == std::string::npos, L"lost+found should have been removed from the volume root");
    }

    WSLC_TEST_METHOD(NamedVolumesGuest)
    {
        ValidateNamedVolumeContract("guest", nullptr, 0);
    }

    WSLC_TEST_METHOD(NamedVolumesStress)
    {
        constexpr unsigned int c_threadCount = 8;
        constexpr unsigned int c_iterationsPerThread = 50;
        const std::string volumeName = "wslc-stress-vol";

        // Best-effort cleanup of any leftover volume from prior runs / on test exit.
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        std::atomic<unsigned int> failures = 0;
        std::vector<std::thread> threads;
        threads.reserve(c_threadCount);

        for (unsigned int t = 0; t < c_threadCount; ++t)
        {
            threads.emplace_back([&]() {
                for (unsigned int i = 0; i < c_iterationsPerThread; ++i)
                {
                    WSLCVolumeOptions volumeOptions{};
                    volumeOptions.Name = volumeName.c_str();
                    volumeOptions.Driver = "guest";

                    WSLCVolumeInformation volInfo{};
                    HRESULT hrCreate = m_defaultSession->CreateVolume(&volumeOptions, &volInfo);
                    if (FAILED(hrCreate) && hrCreate != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
                    {
                        LogError("CreateVolume(%hs) unexpected HR: 0x%08x", volumeName.c_str(), hrCreate);
                        ++failures;
                    }

                    HRESULT hrDelete = m_defaultSession->DeleteVolume(volumeName.c_str());
                    if (FAILED(hrDelete) && hrDelete != WSLC_E_VOLUME_NOT_FOUND)
                    {
                        LogError("DeleteVolume(%hs) unexpected HR: 0x%08x", volumeName.c_str(), hrDelete);
                        ++failures;
                    }
                }
            });
        }

        for (auto& thread : threads)
        {
            thread.join();
        }

        VERIFY_ARE_EQUAL(failures.load(), 0u);

        // Every thread's iteration ends with a Delete, so the globally-last operation across
        // all threads is guaranteed to be a Delete. The volume must therefore not exist in
        // either our cache or docker -- if either disagrees, our state is desynced from docker.

        // Our cache view: InspectVolume must report not-found.
        wil::unique_cotaskmem_ansistring inspectOutput;
        VERIFY_ARE_EQUAL(m_defaultSession->InspectVolume(volumeName.c_str(), &inspectOutput), WSLC_E_VOLUME_NOT_FOUND);

        // Docker's view: `docker volume inspect` must also report not-found (non-zero exit).
        ExpectCommandResult(m_defaultSession.get(), {"/usr/bin/docker", "volume", "inspect", volumeName}, 1);
    }

    // Verifies that a container using a named volume survives a session restart and the volume's data is preserved.
    void ValidateNamedVolumeRecoveryContract(std::string_view driver, const WSLCDriverOption* driverOpts, ULONG driverOptsCount)
    {
        const std::string driverStr(driver);
        const std::string volumeName = std::format("wslc-test-named-volume-{}", driver);
        const std::string containerName = std::format("wslc-test-container-{}", driver);

        // Best-effort cleanup in case prior failed runs left artifacts behind.
        RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        auto cleanup = wil::scope_exit([&]() {
            RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = driverStr.c_str();
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = driverOptsCount;

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // Create a container that uses the named volume and writes a marker.
        {
            WSLCContainerLauncher writer(
                "debian:latest", containerName, {"/bin/sh", "-c", "echo named-volume-recovery >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            writerContainer.SetDeleteOnClose(false);

            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        // Restart the session and verify the container is recovered.
        ResetTestSession();

        auto recoveredContainer = OpenContainer(m_defaultSession.get(), containerName);
        recoveredContainer.SetDeleteOnClose(false);

        // Verify the named volume still contains the marker after restart.
        {
            WSLCContainerLauncher reader(
                "debian:latest", std::format("{}-reader", containerName), {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "named-volume-recovery\n"}});
        }
    }

    WSLC_TEST_METHOD(NamedVolumeRecovery)
    {
        ValidateNamedVolumeRecoveryContract("guest", nullptr, 0);
    }

    WSLC_TEST_METHOD(NamedVolumesVhdSessionRecovery)
    {

        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};
        ValidateNamedVolumeRecoveryContract("vhd", driverOpts, ARRAYSIZE(driverOpts));

        // Re-create the volume (the recovery helper cleans up on exit) so we
        // can test the "delete VHD while session is down" scenario.
        const std::string volumeName = "wslc-test-named-volume-vhd";
        const std::string containerName = "wslc-test-container-vhd";

        // Prune containers on exit so this test doesn't leak "wslc-test-container-vhd" on exit.
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            PruneResult result;
            LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, &result.result));
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // Create a container that depends on the volume so we can verify it
        // gets dropped when the backing .vhdx is removed.
        {
            WSLCContainerLauncher writer("debian:latest", containerName, {"/bin/sh", "-c", "echo vhd-recovery >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            writerContainer.SetDeleteOnClose(false);

            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        {
            auto restartSession = ResetTestSession();

            VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));

            std::error_code error;
            VERIFY_IS_TRUE(std::filesystem::remove(volumeVhdPath, error));
            VERIFY_ARE_EQUAL(error, std::error_code{});
        }

        // The container can still be opened even though its backing volume is gone, so the
        // user is able to inspect and delete it.
        wil::com_ptr<IWSLCContainer> recoveredContainer;
        VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerName.c_str(), &recoveredContainer));

        // Starting it must fail since the referenced volume cannot be brought online.
        VERIFY_ARE_EQUAL(recoveredContainer->Start(WSLCContainerStartFlagsNone, nullptr, nullptr), WSLC_E_VOLUME_NOT_AVAILABLE);
        ValidateCOMErrorMessageContains(wsl::shared::string::MultiByteToWide(volumeName));

        // Inspecting the volume reports the failure via an "Error" entry in its status.
        {
            wil::unique_cotaskmem_ansistring inspectOutput;
            VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(volumeName.c_str(), &inspectOutput));
            auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(inspectOutput.get());
            VERIFY_IS_TRUE(inspect.Status.has_value());
            VERIFY_IS_TRUE(inspect.Status->contains("Error"));

            // The backing .vhdx was deleted, so recovery fails to attach it with ERROR_FILE_NOT_FOUND.
            const auto expectedError = wsl::shared::string::WideToMultiByte(GetErrorString(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
            VERIFY_ARE_EQUAL(inspect.Status->at("Error"), expectedError);
        }

        // The unavailable volume can still be deleted once the container referencing it is removed.
        VERIFY_SUCCEEDED(recoveredContainer->Delete(WSLCDeleteFlagsForce));
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName.c_str()));
    }

    WSLC_TEST_METHOD(NamedVolumeGuestDriverOptsTest)
    {
        const std::string volumeName = "wslc-test-vol";
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        auto expectReject = [&](const WSLCDriverOption* opts, ULONG optsCount, const std::wstring& expectedMessage) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "guest";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&volumeOptions, &volInfo), E_INVALIDARG);
            ValidateCOMErrorMessageContains(expectedMessage);
        };

        auto expectAccept = [&](const WSLCDriverOption* opts, ULONG optsCount) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "guest";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));
        };

        // Allowed: no options (nullptr).
        expectAccept(nullptr, 0);

        // Allowed: type=tmpfs with device=tmpfs.
        {
            WSLCDriverOption opts[] = {{"type", "tmpfs"}, {"device", "tmpfs"}};
            expectAccept(opts, ARRAYSIZE(opts));
        }

        // Allowed: type=tmpfs with device=tmpfs and o= suboptions.
        {
            WSLCDriverOption opts[] = {{"type", "tmpfs"}, {"device", "tmpfs"}, {"o", "size=100m,uid=1000"}};
            expectAccept(opts, ARRAYSIZE(opts));
        }

        // Blocked: type=none (bind mount).
        {
            WSLCDriverOption opts[] = {{"type", "none"}};
            expectReject(opts, ARRAYSIZE(opts), L"unsupported volume driver options: type=none");
        }

        // Blocked: type=nfs.
        {
            WSLCDriverOption opts[] = {{"type", "nfs"}};
            expectReject(opts, ARRAYSIZE(opts), L"unsupported volume driver options: type=nfs");
        }

        // Blocked by Docker: device without type.
        {
            WSLCDriverOption opts[] = {{"device", "/some/path"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }

        // Blocked by Docker: device=tmpfs without type.
        {
            WSLCDriverOption opts[] = {{"device", "tmpfs"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }

        // Blocked by Docker: device and o without type.
        {
            WSLCDriverOption opts[] = {{"device", "tmpfs"}, {"o", "size=100m"}};
            expectReject(opts, ARRAYSIZE(opts), L"create wslc-test-vol: missing required option: \"type\"");
        }
    }

    WSLC_TEST_METHOD(NamedVolumeVhdOptionsParseTest)
    {
        const std::string volumeName = "wslc-volume-name";

        auto validateInvalidOptionsFailure = [&](const WSLCDriverOption* opts,
                                                 ULONG optsCount,
                                                 HRESULT expectedResult,
                                                 const std::optional<std::wstring>& expectedMessage = std::nullopt) {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

            WSLCVolumeOptions volumeOptions{};
            volumeOptions.Name = volumeName.c_str();
            volumeOptions.Driver = "vhd";
            volumeOptions.DriverOpts = opts;
            volumeOptions.DriverOptsCount = optsCount;

            WSLCVolumeInformation volInfo{};
            const auto result = m_defaultSession->CreateVolume(&volumeOptions, &volInfo);

            if (result != expectedResult)
            {
                LogInfo("CreateVolume mismatch result=0x%08x expected=0x%08x", static_cast<unsigned int>(result), static_cast<unsigned int>(expectedResult));
            }

            VERIFY_ARE_EQUAL(result, expectedResult);
            if (expectedMessage.has_value())
            {
                ValidateCOMErrorMessage(expectedMessage);
            }
        };

        // Missing SizeBytes.
        validateInvalidOptionsFailure(nullptr, 0, E_INVALIDARG, L"Missing required option: 'SizeBytes'");

        WSLCDriverOption wrongOption[] = {{"WrongOption", "value"}};
        validateInvalidOptionsFailure(wrongOption, ARRAYSIZE(wrongOption), E_INVALIDARG, L"Missing required option: 'SizeBytes'");

        // Invalid SizeBytes values.
        WSLCDriverOption emptySize[] = {{"SizeBytes", ""}};
        validateInvalidOptionsFailure(emptySize, ARRAYSIZE(emptySize), E_INVALIDARG, L"Invalid value for option 'SizeBytes': ''");

        WSLCDriverOption zeroSize[] = {{"SizeBytes", "0"}};
        validateInvalidOptionsFailure(zeroSize, ARRAYSIZE(zeroSize), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '0'");

        WSLCDriverOption invalidSizeAbc[] = {{"SizeBytes", "abc"}};
        validateInvalidOptionsFailure(
            invalidSizeAbc, ARRAYSIZE(invalidSizeAbc), E_INVALIDARG, L"Invalid value for option 'SizeBytes': 'abc'");

        WSLCDriverOption invalidSizeMixed[] = {{"SizeBytes", "123abc"}};
        validateInvalidOptionsFailure(
            invalidSizeMixed, ARRAYSIZE(invalidSizeMixed), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '123abc'");

        WSLCDriverOption invalidSizeSign[] = {{"SizeBytes", "+-1"}};
        validateInvalidOptionsFailure(
            invalidSizeSign, ARRAYSIZE(invalidSizeSign), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '+-1'");

        WSLCDriverOption invalidSizeOverflow[] = {{"SizeBytes", "18446744073709551616"}};
        validateInvalidOptionsFailure(
            invalidSizeOverflow,
            ARRAYSIZE(invalidSizeOverflow),
            E_INVALIDARG,
            L"Invalid value for option 'SizeBytes': '18446744073709551616'");

        WSLCDriverOption invalidSizeNeg[] = {{"SizeBytes", "-1"}};
        validateInvalidOptionsFailure(
            invalidSizeNeg, ARRAYSIZE(invalidSizeNeg), E_INVALIDARG, L"Invalid value for option 'SizeBytes': '-1'");

        // Invalid Fixed values.
        WSLCDriverOption invalidFixed[] = {{"SizeBytes", "1073741824"}, {"Fixed", "yes"}};
        validateInvalidOptionsFailure(
            invalidFixed, ARRAYSIZE(invalidFixed), E_INVALIDARG, L"Invalid value for option 'Fixed': 'yes'");

        WSLCDriverOption emptyFixed[] = {{"SizeBytes", "1073741824"}, {"Fixed", ""}};
        validateInvalidOptionsFailure(emptyFixed, ARRAYSIZE(emptyFixed), E_INVALIDARG, L"Invalid value for option 'Fixed': ''");

        // Invalid Uid values. Tests pair Uid with a valid Gid because Parse
        // requires both to be present together.
        WSLCDriverOption negUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "-1"}, {"Gid", "0"}};
        validateInvalidOptionsFailure(negUid, ARRAYSIZE(negUid), E_INVALIDARG, L"Invalid value for option 'Uid': '-1'");

        WSLCDriverOption abcUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "abc"}, {"Gid", "0"}};
        validateInvalidOptionsFailure(abcUid, ARRAYSIZE(abcUid), E_INVALIDARG, L"Invalid value for option 'Uid': 'abc'");

        WSLCDriverOption hugeUid[] = {{"SizeBytes", "1073741824"}, {"Uid", "4294967296"}, {"Gid", "0"}}; // 2^32, exceeds uint32_t max
        validateInvalidOptionsFailure(hugeUid, ARRAYSIZE(hugeUid), E_INVALIDARG, L"Invalid value for option 'Uid': '4294967296'");

        // Invalid Gid values.
        WSLCDriverOption negGid[] = {{"SizeBytes", "1073741824"}, {"Uid", "0"}, {"Gid", "-1"}};
        validateInvalidOptionsFailure(negGid, ARRAYSIZE(negGid), E_INVALIDARG, L"Invalid value for option 'Gid': '-1'");

        // Uid without Gid (or vice versa) is rejected.
        WSLCDriverOption uidOnly[] = {{"SizeBytes", "1073741824"}, {"Uid", "1000"}};
        validateInvalidOptionsFailure(uidOnly, ARRAYSIZE(uidOnly), E_INVALIDARG, L"Missing required option: 'Gid'");

        WSLCDriverOption gidOnly[] = {{"SizeBytes", "1073741824"}, {"Gid", "1000"}};
        validateInvalidOptionsFailure(gidOnly, ARRAYSIZE(gidOnly), E_INVALIDARG, L"Missing required option: 'Uid'");

        // Unknown options are rejected (catches typos and unsupported keys).
        WSLCDriverOption unknownOpt[] = {{"SizeBytes", "1073741824"}, {"Bogus", "value"}};
        validateInvalidOptionsFailure(unknownOpt, ARRAYSIZE(unknownOpt), E_INVALIDARG, L"Unknown option: 'Bogus'");
    }

    WSLC_TEST_METHOD(NamedVolumesVhdOwnership)
    {
        // Verify Uid/Gid are baked into the root inode at mkfs time so a
        // non-root container user can write to the volume.
        const std::string volumeName = "wslc-test-vhd-ownership";

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        // nobody/nogroup are typically uid=65534 / gid=65534 on Debian.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}, {"Uid", "65534"}, {"Gid", "65534"}};

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        // A container running as 'nobody' should be able to write to the volume.
        {
            WSLCContainerLauncher writer(
                "debian:latest", "vhd-ownership-writer", {"/bin/sh", "-c", "echo non-root >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);
            writer.SetUser("nobody:nogroup");

            auto writerContainer = writer.Launch(*m_defaultSession);
            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});
        }

        // Verify the file is owned by the same uid/gid as the volume root.
        {
            WSLCContainerLauncher checker(
                "debian:latest", "vhd-ownership-checker", {"/bin/sh", "-c", "stat -c '%u %g' /data && cat /data/marker.txt"});
            checker.AddNamedVolume(volumeName, "/data", true);

            auto checkerContainer = checker.Launch(*m_defaultSession);
            auto checkerProcess = checkerContainer.GetInitProcess();
            ValidateProcessOutput(checkerProcess, {{1, "65534 65534\nnon-root\n"}});
        }
    }

    WSLC_TEST_METHOD(NamedVolumesVhdFixed)
    {
        // Fixed=true produces a .vhdx whose on-disk size is at least SizeBytes.
        const std::string volumeName = "wslc-test-vhd-fixed";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");
        constexpr ULONGLONG c_sizeBytes = 64 * _1MB;

        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

        const auto sizeBytesStr = std::to_string(c_sizeBytes);
        WSLCDriverOption driverOpts[] = {{"SizeBytes", sizeBytesStr.c_str()}, {"Fixed", "true"}};

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Driver = "vhd";
        volumeOptions.DriverOpts = driverOpts;
        volumeOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions, &volInfo));

        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));
        const auto fileSize = std::filesystem::file_size(volumeVhdPath);

        // A dynamic VHD for a 64MB volume is typically a few MB; a fixed VHD
        // pre-allocates the full payload (>= SizeBytes).
        VERIFY_IS_GREATER_THAN_OR_EQUAL(fileSize, c_sizeBytes);
    }

    WSLC_TEST_METHOD(ListAndInspectNamedVolumesTest)
    {
        const std::string vhdVolumeName = "wsla-test-vol-vhd";
        const std::string guestVolumeName = "wsla-test-vol-guest";

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(vhdVolumeName.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(guestVolumeName.c_str()));
        });

        // Verify empty list is returned when no volumes exist.
        VERIFY_IS_TRUE(ListVolumes().empty());

        // Create a VHD volume and verify list returns one entry.
        WSLCDriverOption driverOpts[] = {{"SizeBytes", "1073741824"}};

        WSLCVolumeOptions vhdOptions{};
        vhdOptions.Name = vhdVolumeName.c_str();
        vhdOptions.Driver = "vhd";
        vhdOptions.DriverOpts = driverOpts;
        vhdOptions.DriverOptsCount = ARRAYSIZE(driverOpts);

        WSLCVolumeInformation volInfo{};
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&vhdOptions, &volInfo));

        wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> volumes;
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(nullptr, 0, volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(std::string(volumes[0].Name), vhdVolumeName);
        VERIFY_ARE_EQUAL(std::string(volumes[0].Driver), std::string("vhd"));

        // Verify that a guest volume cannot be created with the same name as an existing vhd volume.
        WSLCVolumeOptions duplicateGuestOptions{};
        duplicateGuestOptions.Name = vhdVolumeName.c_str();
        duplicateGuestOptions.Driver = "guest";
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&duplicateGuestOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Create a guest volume and verify both drivers show up in the list.
        WSLCVolumeOptions guestOptions{};
        guestOptions.Name = guestVolumeName.c_str();
        guestOptions.Driver = "guest";
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&guestOptions, &volInfo));

        // Verify that a vhd volume cannot be created with the same name as an existing guest volume.
        WSLCVolumeOptions duplicateVhdOptions{};
        duplicateVhdOptions.Name = guestVolumeName.c_str();
        duplicateVhdOptions.Driver = "vhd";
        duplicateVhdOptions.DriverOpts = driverOpts;
        duplicateVhdOptions.DriverOptsCount = ARRAYSIZE(driverOpts);
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&duplicateVhdOptions, &volInfo), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(nullptr, 0, volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(2u, volumes.size());

        std::map<std::string, std::string> namesToDrivers;
        for (const auto& v : volumes)
        {
            namesToDrivers.emplace(v.Name, v.Driver);
        }

        VERIFY_ARE_EQUAL(namesToDrivers[vhdVolumeName], std::string("vhd"));
        VERIFY_ARE_EQUAL(namesToDrivers[guestVolumeName], std::string("guest"));

        // Verify InspectVolume returns correct details for the VHD volume (driver opts present).
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(vhdVolumeName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto vhdInspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
        VERIFY_ARE_EQUAL(vhdInspect.Name, vhdVolumeName);
        VERIFY_ARE_EQUAL(vhdInspect.Driver, std::string("vhd"));
        VERIFY_IS_TRUE(vhdInspect.DriverOpts.contains("SizeBytes"));

        // Verify InspectVolume returns correct details for the guest volume (no driver opts).
        output.reset();
        VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(guestVolumeName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto guestInspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
        VERIFY_ARE_EQUAL(guestInspect.Name, guestVolumeName);
        VERIFY_ARE_EQUAL(guestInspect.Driver, std::string("guest"));
        VERIFY_IS_TRUE(guestInspect.DriverOpts.empty());

        // Verify InspectVolume fails for a non-existent volume.
        output.reset();
        VERIFY_ARE_EQUAL(m_defaultSession->InspectVolume("does-not-exist", &output), WSLC_E_VOLUME_NOT_FOUND);

        // Delete the VHD volume and verify only the guest volume remains.
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(vhdVolumeName.c_str()));
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(nullptr, 0, volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(std::string(volumes[0].Name), guestVolumeName);
        VERIFY_ARE_EQUAL(std::string(volumes[0].Driver), std::string("guest"));
    }

    WSLC_TEST_METHOD(ListVolumesFilters)
    {
        const std::string vhdA = "wslc-list-vhd-a";
        const std::string vhdB = "wslc-list-vhd-b";
        const std::string guestA = "wslc-list-guest-a";
        const std::string guestB = "wslc-list-guest-b";
        const std::string otherName = "wslc-list-other-name";
        const std::string emptyValVol = "wslc-list-empty-val";

        const std::vector<WSLCDriverOption> vhdOpts = {{"SizeBytes", "1073741824"}};

        auto cleanup = wil::scope_exit([&]() {
            for (const auto& name : {vhdA, vhdB, guestA, guestB, otherName, emptyValVol})
            {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(name.c_str()));
            }
        });

        CreateNamedVolume(vhdA, "vhd", {{"env", "prod"}}, vhdOpts);
        CreateNamedVolume(vhdB, "vhd", {{"env", "test"}, {"tier", "db"}}, vhdOpts);
        CreateNamedVolume(guestA, "guest", {{"env", "prod"}});
        CreateNamedVolume(guestB, "guest");
        CreateNamedVolume(otherName, "guest", {{"env", "test"}});
        CreateNamedVolume(emptyValVol, "guest", {{"marker", ""}});

        auto expectListFails = [&](HRESULT expected, const std::vector<WSLCFilter>& filters) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> volumes;
            VERIFY_ARE_EQUAL(
                expected, m_defaultSession->ListVolumes(filtersPtr, filtersCount, volumes.addressof(), volumes.size_address<ULONG>()));
        };

        auto expectList = [&](const std::vector<std::string>& expected,
                              const std::vector<WSLCFilter>& filters = {},
                              const std::source_location& source = std::source_location::current()) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> volumes;
            VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(filtersPtr, filtersCount, volumes.addressof(), volumes.size_address<ULONG>()));

            std::vector<std::string> names;
            for (const auto& v : volumes)
            {
                names.emplace_back(v.Name);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        const std::vector<std::string> all{vhdA, vhdB, guestA, guestB, otherName, emptyValVol};

        // No filter returns every volume.
        expectList(all);

        // Filter by driver name.
        expectList({vhdA, vhdB}, {{"driver", "vhd"}});
        expectList({guestA, guestB, otherName, emptyValVol}, {{"driver", "guest"}});
        expectList({}, {{"driver", "nonexistent"}});

        // Filter by volume name.
        expectList({vhdA, vhdB}, {{"name", "vhd"}});

        // Anchored regex matches exactly one volume.
        const auto anchoredVhdA = "^" + vhdA + "$";
        expectList({vhdA}, {{"name", anchoredVhdA.c_str()}});

        // Regex name filter.
        expectList({vhdA, vhdB}, {{"name", "vhd-."}});

        // Filter by label key (any value matches): label=<key> form.
        expectList({vhdA, vhdB, guestA, otherName}, {{"label", "env"}});

        // Filter by label key=value.
        expectList({vhdA, guestA}, {{"label", "env=prod"}});

        // Multiple labels are AND'ed together.
        expectList({vhdB}, {{"label", "env=test"}, {"label", "tier=db"}});

        // Unknown label key matches nothing.
        expectList({}, {{"label", "nope"}});

        // Unknown name matches nothing.
        expectList({}, {{"name", "nope"}});

        // Combined driver + name + label filter.
        expectList({vhdA}, {{"driver", "vhd"}, {"name", "a"}, {"label", "env=prod"}});

        // Dangling filter is supported by docker. All our named test volumes
        // are unused, so they are all dangling; combine with a name prefix to
        // exclude any leftover dangling volumes from other tests.
        expectList(all, {{"dangling", "true"}, {"name", "^wslc-list-"}});

        // label=<key> (key-only) matches the volume with the marker label regardless of stored value.
        expectList({emptyValVol}, {{"label", "marker"}});

        // label=<key>= (explicit empty value) matches only volumes whose stored value is also the empty string.
        expectList({emptyValVol}, {{"label", "marker="}});

        // No volume stores `env` with an empty value, so env= matches nothing.
        expectList({}, {{"label", "env="}});

        // env (key-only) matches every volume that has the key, regardless of its stored value.
        expectList({vhdA, vhdB, guestA, otherName}, {{"label", "env"}});

        // Unknown filter keys are rejected.
        expectListFails(E_INVALIDARG, {{"bogus", "x"}});

        // Null filter key/value is rejected.
        expectListFails(E_POINTER, {{nullptr, "anything"}});
        expectListFails(E_POINTER, {{"label", nullptr}});
    }

    WSLC_TEST_METHOD(PruneVolumesTest)
    {
        auto expectPrune = [&](const std::vector<std::string>& expected,
                               const std::vector<WSLCFilter>& filters = {},
                               const std::source_location& source = std::source_location::current()) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;
            VERIFY_SUCCEEDED(m_defaultSession->PruneVolumes(
                filtersPtr, filtersCount, nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));

            std::vector<std::string> names;
            for (const auto& n : deleted)
            {
                names.emplace_back(n);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        // Prune with no eligible volumes (none created yet) returns an empty set.
        expectPrune({}, {{"all", "true"}});

        // Default (no all=true) only prunes anonymous volumes; with none present, returns empty.
        expectPrune({});

        // all=true prunes unused named guest volumes.
        {
            const std::string a = "wslc-prune-guest-a";
            const std::string b = "wslc-prune-guest-b";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(a.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(b.c_str()));
            });

            CreateNamedVolume(a, "guest");
            CreateNamedVolume(b, "guest");

            expectPrune({a, b}, {{"all", "true"}});

            auto volumes = ListVolumes();
            VERIFY_IS_FALSE(volumes.contains(a));
            VERIFY_IS_FALSE(volumes.contains(b));
        }

        // In-use volume is not pruned.
        {
            const std::string name = "wslc-prune-in-use";
            CreateNamedVolume(name, "guest");

            auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(name.c_str())); });

            WSLCContainerLauncher launcher("debian:latest", "wslc-prune-in-use-holder", {"sleep", "99999"});
            launcher.AddNamedVolume(name, "/data", false);
            auto container = launcher.Launch(*m_defaultSession);

            expectPrune({}, {{"all", "true"}});
            VERIFY_IS_TRUE(ListVolumes().contains(name));

            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGKILL));
        }

        // Label filter (present, key=value).
        {
            const std::string labeled = "wslc-prune-labeled";
            const std::string unlabeled = "wslc-prune-unlabeled";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(unlabeled.c_str()));
            });

            CreateNamedVolume(labeled, "guest", {{"wslc-prune-test", "yes"}});
            CreateNamedVolume(unlabeled, "guest");

            expectPrune({labeled}, {{"all", "true"}, {"label", "wslc-prune-test=yes"}});
        }

        // Label filter (present, key only).
        {
            const std::string labeled = "wslc-prune-keyonly";
            const std::string unlabeled = "wslc-prune-keyonly-no";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(unlabeled.c_str()));
            });

            CreateNamedVolume(labeled, "guest", {{"wslc-prune-keyonly", "anything"}});
            CreateNamedVolume(unlabeled, "guest");

            // Value without '=' matches any volume with the key (Docker `label=key`).
            expectPrune({labeled}, {{"all", "true"}, {"label", "wslc-prune-keyonly"}});
        }

        // Label filter (absent, key only).
        {
            const std::string keep = "wslc-prune-keep";
            const std::string drop = "wslc-prune-drop";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(keep.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(drop.c_str()));
            });

            CreateNamedVolume(keep, "guest", {{"wslc-prune-keep", "yes"}});
            CreateNamedVolume(drop, "guest");

            // `label!` filters out volumes that have the key (Docker `label!=key`).
            expectPrune({drop}, {{"all", "true"}, {"label!", "wslc-prune-keep"}});
        }

        // VHD volumes are not pruned (docker skips bind-mount volumes).
        {
            const std::string vhdName = "wslc-prune-vhd-skip";
            const std::string guestName = "wslc-prune-vhd-skip-guest";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(vhdName.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(guestName.c_str()));
            });

            CreateNamedVolume(vhdName, "vhd", {}, {{"SizeBytes", "1073741824"}});
            CreateNamedVolume(guestName, "guest");

            expectPrune({guestName}, {{"all", "true"}});

            VERIFY_IS_TRUE(ListVolumes().contains(vhdName));
        }

        // ListVolumes / InspectVolume reflect prune results.
        {
            const std::string name = "wslc-prune-listsync";
            CreateNamedVolume(name, "guest");

            expectPrune({name}, {{"all", "true"}});
            VERIFY_IS_FALSE(ListVolumes().contains(name));
        }

        // Filter with null Key rejected.
        {
            WSLCFilter filters[] = {{nullptr, "true"}};

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;

            VERIFY_ARE_EQUAL(
                E_POINTER,
                m_defaultSession->PruneVolumes(
                    filters, ARRAYSIZE(filters), nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));
        }

        // Filter with null Value rejected.
        {
            WSLCFilter filters[] = {{"label", nullptr}};

            wil::unique_cotaskmem_array_ptr<WSLCVolumeName> deleted;
            ULONGLONG spaceReclaimed = 0;

            VERIFY_ARE_EQUAL(
                E_POINTER,
                m_defaultSession->PruneVolumes(
                    filters, ARRAYSIZE(filters), nullptr, deleted.addressof(), deleted.size_address<ULONG>(), &spaceReclaimed));
        }
    }

    WSLC_TEST_METHOD(NetworkCreateDeleteListTest)
    {
        const std::string networkName = "test-network";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        // List should start empty.
        wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(0u, networks.size());

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        // Verify it appears in the list with correct fields.
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, networks.size());
        VERIFY_ARE_EQUAL(networkName, std::string(networks[0].Name));
        VERIFY_ARE_EQUAL(std::string("bridge"), std::string(networks[0].Driver));
        VERIFY_IS_TRUE(strlen(networks[0].Id) > 0);

        // Duplicate name should fail.
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_defaultSession->CreateNetwork(&options, nullptr));

        cleanup.release();
        VERIFY_SUCCEEDED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        // List should be empty again.
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(0u, networks.size());

        // Delete non-existent should fail.
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->DeleteNetwork(networkName.c_str()));
    }

    void CreateNamedNetwork(const std::string& Name, const std::vector<WSLCLabel>& Labels = {})
    {
        WSLCNetworkOptions options{};
        options.Name = Name.c_str();
        options.Driver = "bridge";
        options.Labels = Labels.empty() ? nullptr : Labels.data();
        options.LabelsCount = static_cast<ULONG>(Labels.size());

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));
    }

    WSLC_TEST_METHOD(PruneNetworksTest)
    {
        auto expectPrune = [&](const std::vector<std::string>& expected,
                               const std::vector<WSLCFilter>& filters = {},
                               const std::source_location& source = std::source_location::current()) {
            const WSLCFilter* filtersPtr = filters.empty() ? nullptr : filters.data();
            const ULONG filtersCount = static_cast<ULONG>(filters.size());

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_SUCCEEDED(m_defaultSession->PruneNetworks(filtersPtr, filtersCount, deleted.addressof(), deleted.size_address<ULONG>()));

            std::vector<std::string> names;
            for (const auto& n : deleted)
            {
                names.emplace_back(n);
            }

            VerifyAreEqualUnordered(expected, names, source);
        };

        // Prune with no managed networks present returns empty.
        expectPrune({});

        // Prune removes unused managed networks.
        {
            const std::string a = "wslc-prune-net-a";
            const std::string b = "wslc-prune-net-b";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(a.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(b.c_str()));
            });

            CreateNamedNetwork(a);
            CreateNamedNetwork(b);

            expectPrune({a, b});

            wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
            VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
            for (const auto& n : networks)
            {
                VERIFY_ARE_NOT_EQUAL(a, std::string(n.Name));
                VERIFY_ARE_NOT_EQUAL(b, std::string(n.Name));
            }

            cleanup.release();
        }

        // Label filter (key=value).
        {
            const std::string labeled = "wslc-prune-net-labeled";
            const std::string unlabeled = "wslc-prune-net-unlabeled";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(labeled.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(unlabeled.c_str()));
            });

            CreateNamedNetwork(labeled, {{"wslc-prune-net-test", "yes"}});
            CreateNamedNetwork(unlabeled);

            expectPrune({labeled}, {{"label", "wslc-prune-net-test=yes"}});

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(unlabeled.c_str()));
            cleanup.release();
        }

        // Label filter (negation).
        {
            const std::string keep = "wslc-prune-net-keep";
            const std::string drop = "wslc-prune-net-drop";

            auto cleanup = wil::scope_exit([&]() {
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(keep.c_str()));
                LOG_IF_FAILED(m_defaultSession->DeleteNetwork(drop.c_str()));
            });

            CreateNamedNetwork(keep, {{"wslc-prune-net-keep", "yes"}});
            CreateNamedNetwork(drop);

            expectPrune({drop}, {{"label!", "wslc-prune-net-keep"}});

            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(keep.c_str()));
            cleanup.release();
        }

        // Filter with null Key rejected.
        {
            WSLCFilter filters[] = {{nullptr, "true"}};

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_ARE_EQUAL(
                E_POINTER, m_defaultSession->PruneNetworks(filters, ARRAYSIZE(filters), deleted.addressof(), deleted.size_address<ULONG>()));
        }

        // Filter with null Value rejected.
        {
            WSLCFilter filters[] = {{"label", nullptr}};

            wil::unique_cotaskmem_array_ptr<WSLCNetworkName> deleted;
            VERIFY_ARE_EQUAL(
                E_POINTER, m_defaultSession->PruneNetworks(filters, ARRAYSIZE(filters), deleted.addressof(), deleted.size_address<ULONG>()));
        }
    }

    WSLC_TEST_METHOD(NetworkCreateWithSubnetTest)
    {
        const std::string networkName = "subnet-test-net";
        const std::string subnet = "172.28.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
    }

    WSLC_TEST_METHOD(NetworkCreateInternalTest)
    {
        const std::string networkName = "internal-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Internal = TRUE;

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(networkName, inspect.Name);
        VERIFY_IS_TRUE(inspect.Internal);
    }

    WSLC_TEST_METHOD(NetworkCreateWithLabelsTest)
    {
        const std::string networkName = "labels-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCLabel labels[] = {
            {.Key = "com.example.env", .Value = "test"},
            {.Key = "com.example.team", .Value = "infra"},
        };

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        options.Labels = labels;
        options.LabelsCount = ARRAYSIZE(labels);

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, networks.size());
        VERIFY_ARE_EQUAL(networkName, std::string(networks[0].Name));
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidDriverAndOptionTest)
    {
        const std::string networkName = "bad-network-create-input";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";

        auto verifyInvalid = [&](PCWSTR expectedMessage) {
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
            ValidateCOMErrorMessageContains(expectedMessage);
        };

        // Invalid drivers (unknown, wrong case, empty)
        for (const char* driver : {"overlay", "Bridge", ""})
        {
            options.Driver = driver;
            verifyInvalid(L"Unsupported network driver:");
        }

        // Gateway specified without Subnet
        {
            options.Driver = "bridge";
            options.Subnet = nullptr;
            options.Gateway = "172.44.0.1";
            verifyInvalid(L"--subnet");
        }
    }

    WSLC_TEST_METHOD(NetworkCreateDefaultDriverTest)
    {
        const std::string networkName = "default-driver-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = nullptr;
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, networks.size());
        VERIFY_ARE_EQUAL(networkName, std::string(networks[0].Name));
        VERIFY_ARE_EQUAL(std::string("bridge"), std::string(networks[0].Driver));
    }

    WSLC_TEST_METHOD(NetworkCreateReservedNameTest)
    {
        WSLCNetworkOptions options{};
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        options.Name = "bridge";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"bridge");

        options.Name = "host";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"host");

        options.Name = "none";
        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"none");
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidNameTest)
    {
        WSLCNetworkOptions options{};
        options.Name = "invalid name!";
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid name!");
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidSubnetTest)
    {
        const std::string networkName = "bad-subnet-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = "not-a-cidr";

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid subnet");

        wil::unique_cotaskmem_ansistring output;
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->InspectNetwork(networkName.c_str(), &output));
    }

    WSLC_TEST_METHOD(NetworkCreateInvalidGatewayTest)
    {
        const std::string networkName = "bad-gateway-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = "172.27.0.0/16";
        options.Gateway = "999.999.999.999";

        VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateNetwork(&options, nullptr));
        ValidateCOMErrorMessageContains(L"invalid gateway");

        wil::unique_cotaskmem_ansistring output;
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, m_defaultSession->InspectNetwork(networkName.c_str(), &output));
    }

    WSLC_TEST_METHOD(NetworkCreateWithGatewayTest)
    {
        const std::string networkName = "gateway-test-net";
        const std::string subnet = "172.31.0.0/16";
        const std::string gateway = "172.31.0.1";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        options.Gateway = gateway.c_str();

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
        VERIFY_ARE_EQUAL(gateway, inspect.IPAM.Config->at(0).Gateway);
    }

    WSLC_TEST_METHOD(NetworkCreateWithArbitraryDriverOptsTest)
    {
        const std::string networkName = "arbitrary-opts-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCDriverOption opts[] = {{"my.abc.key", "mygod"}, {"com.example.flag", "1"}};

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = opts;
        options.DriverOptsCount = ARRAYSIZE(opts);

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.Options.has_value());
        VERIFY_IS_TRUE(inspect.Options->contains("my.abc.key"));
        VERIFY_IS_TRUE(inspect.Options->contains("com.example.flag"));
        VERIFY_ARE_EQUAL(std::string("mygod"), inspect.Options->at("my.abc.key"));
        VERIFY_ARE_EQUAL(std::string("1"), inspect.Options->at("com.example.flag"));
    }

    WSLC_TEST_METHOD(NetworkSessionRecoveryTest)
    {
        const std::string networkName = "recovery-test-net";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCDriverOption recoveryOpts[] = {{"recovery.test.key", "preserved"}};

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = recoveryOpts;
        options.DriverOptsCount = ARRAYSIZE(recoveryOpts);
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        // Reset the session (simulates session restart).
        ResetTestSession();

        wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, networks.size());
        VERIFY_ARE_EQUAL(networkName, std::string(networks[0].Name));
        VERIFY_ARE_EQUAL(std::string("bridge"), std::string(networks[0].Driver));
        VERIFY_IS_TRUE(strlen(networks[0].Id) > 0);

        // Verify arbitrary driver options survive session recovery.
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_IS_TRUE(inspect.Options.has_value());
        VERIFY_IS_TRUE(inspect.Options->contains("recovery.test.key"));
        VERIFY_ARE_EQUAL(std::string("preserved"), inspect.Options->at("recovery.test.key"));
    }

    WSLC_TEST_METHOD(NetworkMultipleCreateListDeleteTest)
    {
        const std::string networkNameA = "net-a";
        const std::string networkNameB = "net-b";
        const std::string networkNameC = "net-c";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameA.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameC.c_str()));

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameA.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkNameC.c_str()));
        });

        WSLCNetworkOptions optionsA{};
        optionsA.Name = networkNameA.c_str();
        optionsA.Driver = "bridge";
        optionsA.DriverOpts = nullptr;
        optionsA.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsA, nullptr));

        WSLCNetworkOptions optionsB{};
        optionsB.Name = networkNameB.c_str();
        optionsB.Driver = "bridge";
        optionsB.Subnet = "172.29.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsB, nullptr));

        WSLCNetworkOptions optionsC{};
        optionsC.Name = networkNameC.c_str();
        optionsC.Driver = "bridge";
        optionsC.Internal = TRUE;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&optionsC, nullptr));

        wil::unique_cotaskmem_array_ptr<WSLCNetworkInformation> networks;
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(3u, networks.size());

        VERIFY_SUCCEEDED(m_defaultSession->DeleteNetwork(networkNameB.c_str()));
        VERIFY_SUCCEEDED(m_defaultSession->ListNetworks(networks.addressof(), networks.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(2u, networks.size());
    }

    WSLC_TEST_METHOD(NetworkInspectTest)
    {
        const std::string networkName = "test-inspect-network";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.DriverOpts = nullptr;
        options.DriverOptsCount = 0;
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(inspect.Name, networkName);
        VERIFY_ARE_EQUAL(inspect.Driver, std::string("bridge"));
        VERIFY_IS_FALSE(inspect.Id.empty());
        VERIFY_IS_FALSE(inspect.Internal);
    }

    WSLC_TEST_METHOD(NetworkInspectWithSubnetTest)
    {
        const std::string networkName = "test-inspect-subnet-net";
        const std::string subnet = "172.30.0.0/16";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCNetworkOptions options{};
        options.Name = networkName.c_str();
        options.Driver = "bridge";
        options.Subnet = subnet.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&options, nullptr));

        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectNetwork(networkName.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::Network>(output.get());
        VERIFY_ARE_EQUAL(inspect.Name, networkName);
        VERIFY_ARE_EQUAL(inspect.Driver, std::string("bridge"));
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(subnet, inspect.IPAM.Config->at(0).Subnet);
    }

    WSLC_TEST_METHOD(NetworkInspectNotFoundTest)
    {
        wil::unique_cotaskmem_ansistring output;
        auto hr = m_defaultSession->InspectNetwork("nonexistent-network", &output);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, hr);
        ValidateCOMErrorMessageContains(L"nonexistent-network");
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

        // Validate that relative tmpfs paths are rejected by Docker.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-tmpfs-relative", {"/bin/cat"});
            launcher.AddTmpfs("relative-path", "");

            auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(hresult, E_FAIL);

            ValidateCOMErrorMessage(L"invalid mount path: 'relative-path' mount path must be absolute");
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
            ULONGLONG runningStateChangedAt{};
            ULONGLONG runningCreatedAt{};
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

                auto now = static_cast<ULONGLONG>(time(nullptr));
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
                    // TODO: calling Kill() here hangs since Stop() holds the container lock.
                    // Update this once fixed to:
                    // LOG_IF_FAILED(container.Get().Kill(WSLCSignalSIGKILL));

                    LOG_IF_FAILED(initProcess.Get().Signal(WSLCSignalSIGKILL));

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

            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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

            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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

            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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

    WSLC_TEST_METHOD(ContainerNetwork)
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

        // Verify that containers launch successfully when host and none are used as network modes
        // TODO: Test bridge network container launch when VHD with bridge cni is ready
        // TODO: Add port mapping related tests when port mapping is implemented
        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "host");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto details = container.Inspect();
            VERIFY_ARE_EQUAL(details.HostConfig.NetworkMode, "host");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "none");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "none");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }

        {
            // Unknown network names are rejected as "network not found".
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "no-such-network");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(retVal.first, WSLC_E_NETWORK_NOT_FOUND);
        }

        {
            WSLCContainerLauncher launcher("debian:latest", "test-network", {"sleep", "99999"}, {}, "bridge");

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, "bridge");

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-network", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            expectContainerList({});
        }
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkTest)
    {
        const std::string networkName = "custom-net-test";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.35.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-custom-network", {"sleep", "99999"}, {}, std::string(networkName));

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkNotFoundTest)
    {
        WSLCContainerLauncher launcher("debian:latest", "test-custom-network-notfound", {"sleep", "99999"}, {}, std::string("nonexistent-net"));

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessageContains(L"nonexistent-net");
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkEmptyNameTest)
    {
        // Empty entry in the Networks array must be rejected.
        LPCSTR args[] = {"sleep", "99999"};
        WSLCNetworkConnection emptyConnection{};
        emptyConnection.NetworkName = ""; // empty name — should be rejected

        WSLCContainerOptions options{};
        options.Image = "debian:latest";
        options.Name = "test-custom-network-empty";
        options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
        options.ContainerNetwork.Networks = &emptyConnection;
        options.ContainerNetwork.NetworksCount = 1;

        wil::com_ptr<IWSLCContainer> container;
        auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
        VERIFY_ARE_EQUAL(E_INVALIDARG, hr);
        ValidateCOMErrorMessageContains(L"Network name");
    }

    WSLC_TEST_METHOD(ContainerNetworkEndpointSettingsNotImplementedTest)
    {
        // Per-endpoint Settings (Aliases, IPAMConfig, etc.) are reserved for a future PR.
        // Until that lands, any non-empty Settings payload must be rejected with E_NOTIMPL
        // so callers don't silently lose data.
        const std::string networkName = "custom-net-settings";
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));
        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        LPCSTR args[] = {"sleep", "99999"};
        KeyValuePair settings[] = {{"Aliases", "my-alias"}};
        WSLCNetworkConnection connection{};
        connection.NetworkName = networkName.c_str();
        connection.Settings = settings;
        connection.SettingsCount = ARRAYSIZE(settings);

        WSLCContainerOptions options{};
        options.Image = "debian:latest";
        options.Name = "test-endpoint-settings";
        options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
        options.ContainerNetwork.NetworkMode = "bridge";
        options.ContainerNetwork.Networks = &connection;
        options.ContainerNetwork.NetworksCount = 1;

        wil::com_ptr<IWSLCContainer> container;
        auto hr = m_defaultSession->CreateContainer(&options, nullptr, &container);
        VERIFY_ARE_EQUAL(E_NOTIMPL, hr);
        ValidateCOMErrorMessage(L"Endpoint settings are not yet supported (network 'custom-net-settings').");
    }

    WSLC_TEST_METHOD(ContainerUnsupportedColonNetworkModeRejectedTest)
    {
        // Only `container:` is a recognized colon-prefixed mode. Anything else
        // (`service:foo`, `ns:bar`, ...) must be rejected at the WSLC layer rather
        // than being passed through to Docker or interpreted as a user-defined name.
        WSLCContainerLauncher launcher("debian:latest", "test-unsupported-colon", {"sleep", "99999"}, {}, std::string("service:foo"));

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessageContains(L"service:foo");
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkMultipleContainersTest)
    {
        const std::string networkName = "custom-net-multi";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.36.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher1("debian:latest", "test-custom-multi-1", {"sleep", "99999"}, {}, std::string(networkName));

        WSLCContainerLauncher launcher2("debian:latest", "test-custom-multi-2", {"sleep", "99999"}, {}, std::string(networkName));

        auto container1 = launcher1.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container1.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container1.Inspect().HostConfig.NetworkMode, networkName);

        auto container2 = launcher2.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container2.State(), WslcContainerStateRunning);
        VERIFY_ARE_EQUAL(container2.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkDeleteWhileInUseTest)
    {
        const std::string networkName = "custom-net-inuse";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.37.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-custom-net-inuse", {"sleep", "99999"}, {}, std::string(networkName));

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), m_defaultSession->DeleteNetwork(networkName.c_str()));
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkPortMappingTest)
    {
        const std::string networkName = "custom-net-ports";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.38.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher(
            "python:3.12-alpine", "test-custom-net-ports", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, std::string(networkName));
        launcher.AddPort(1251, 8000, AF_INET);

        auto container = launcher.Launch(*m_defaultSession);
        auto initProcess = container.GetInitProcess();
        WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

        ExpectHttpResponse(L"http://127.0.0.1:1251", 200);
    }

    WSLC_TEST_METHOD(ContainerCustomNetworkRecoveryTest)
    {
        const std::string networkName = "custom-net-recovery";
        const std::string containerName = "test-custom-net-recovery";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str()));

        WSLCNetworkOptions networkOptions{};
        networkOptions.Name = networkName.c_str();
        networkOptions.Driver = "bridge";
        networkOptions.Subnet = "172.39.0.0/16";

        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&networkOptions, nullptr));

        auto networkCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, std::string(networkName));

        {
            auto container = launcher.Create(*m_defaultSession);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
        }

        // Restart the session and verify the container is recovered with its custom network.
        ResetTestSession();

        auto recoveredContainer = OpenContainer(m_defaultSession.get(), containerName);

        VERIFY_ARE_EQUAL(recoveredContainer.State(), WslcContainerStateCreated);
        VERIFY_SUCCEEDED(recoveredContainer.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));
        VERIFY_ARE_EQUAL(recoveredContainer.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(recoveredContainer.Inspect().HostConfig.NetworkMode, networkName);
    }

    WSLC_TEST_METHOD(ContainerMultipleNetworksTest)
    {
        const std::string primaryNetwork = "multi-net-primary";
        const std::string additionalNetwork = "multi-net-additional";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));

        WSLCNetworkOptions primaryNetOpts{};
        primaryNetOpts.Name = primaryNetwork.c_str();
        primaryNetOpts.Driver = "bridge";
        primaryNetOpts.Subnet = "172.40.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&primaryNetOpts, nullptr));
        auto primaryCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCNetworkOptions additionalNetOpts{};
        additionalNetOpts.Name = additionalNetwork.c_str();
        additionalNetOpts.Driver = "bridge";
        additionalNetOpts.Subnet = "172.41.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&additionalNetOpts, nullptr));
        auto additionalCleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(additionalNetwork);

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        auto inspect = container.Inspect();
        VERIFY_ARE_EQUAL(inspect.HostConfig.NetworkMode, primaryNetwork);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(primaryNetwork));
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(additionalNetwork));
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(primaryNetwork).IPAddress.empty());
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(additionalNetwork).IPAddress.empty());
    }

    WSLC_TEST_METHOD(ContainerDuplicateNetworkRejectedTest)
    {
        const std::string primaryNetwork = "multi-net-dup-primary";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));

        WSLCNetworkOptions netOpts{};
        netOpts.Name = primaryNetwork.c_str();
        netOpts.Driver = "bridge";
        netOpts.Subnet = "172.48.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-dup-reject", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(primaryNetwork);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(
            std::format(L"Duplicate network: '{}'", std::wstring(primaryNetwork.begin(), primaryNetwork.end())).c_str());
    }

    WSLC_TEST_METHOD(ContainerAdditionalNetworkNotFoundRejectedTest)
    {
        const std::string primaryNetwork = "multi-net-notfound-primary";
        const std::string missingNetwork = "multi-net-nonexistent";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(missingNetwork.c_str()));

        WSLCNetworkOptions netOpts{};
        netOpts.Name = primaryNetwork.c_str();
        netOpts.Driver = "bridge";
        netOpts.Subnet = "172.49.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-notfound-reject", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(missingNetwork);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessage(
            std::format(L"Network not found: '{}'", std::wstring(missingNetwork.begin(), missingNetwork.end())).c_str());
    }

    WSLC_TEST_METHOD(ContainerBridgedPrimaryDuplicateNetworkRejectedTest)
    {
        // Verifies that passing the primary bridge network as an additional network is caught as a duplicate.
        WSLCContainerLauncher launcher("debian:latest", "test-bridge-dup-reject", {"sleep", "99999"}, {}, "bridge");
        launcher.AddAdditionalNetwork("bridge");

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(L"Duplicate network: 'bridge'");
    }

    WSLC_TEST_METHOD(ContainerAdditionalNetworkMalformedNameTest)
    {
        // Most malformed names fall through to the network lookup.

        // Invalid character.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-char", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork("bad/name");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
            ValidateCOMErrorMessage(L"Network not found: 'bad/name'");
        }

        // Empty name.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-empty", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork("");

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
            ValidateCOMErrorMessage(L"Network name cannot be empty.");
        }

        // Name exceeds WSLC_MAX_NETWORK_NAME_LENGTH (255).
        {
            const std::string tooLongName(WSLC_MAX_NETWORK_NAME_LENGTH + 1, 'a');

            WSLCContainerLauncher launcher("debian:latest", "test-invalid-name-long", {"sleep", "99999"}, {}, "bridge");
            launcher.AddAdditionalNetwork(tooLongName);

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(WSLC_E_NETWORK_NOT_FOUND, retVal.first);
            ValidateCOMErrorMessage(std::format(L"Network not found: '{}'", std::wstring(tooLongName.begin(), tooLongName.end())).c_str());
        }
    }

    WSLC_TEST_METHOD(ContainerDeleteAdditionalNetworkWhileInUseTest)
    {
        const std::string primaryNetwork = "multi-net-del-primary";
        const std::string additionalNetwork = "multi-net-del-additional";

        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str()));
        LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));

        WSLCNetworkOptions primaryNetOpts{};
        primaryNetOpts.Name = primaryNetwork.c_str();
        primaryNetOpts.Driver = "bridge";
        primaryNetOpts.Subnet = "172.50.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&primaryNetOpts, nullptr));
        auto primaryCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(primaryNetwork.c_str())); });

        WSLCNetworkOptions additionalNetOpts{};
        additionalNetOpts.Name = additionalNetwork.c_str();
        additionalNetOpts.Driver = "bridge";
        additionalNetOpts.Subnet = "172.51.0.0/16";
        VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&additionalNetOpts, nullptr));
        auto additionalCleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(additionalNetwork.c_str())); });

        WSLCContainerLauncher launcher("debian:latest", "test-multi-net-del", {"sleep", "99999"}, {}, std::string(primaryNetwork));
        launcher.AddAdditionalNetwork(additionalNetwork);

        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), m_defaultSession->DeleteNetwork(additionalNetwork.c_str()));
    }

    WSLC_TEST_METHOD(ConnectDisconnectContainerNetworkTest)
    {
        auto launchContainer = [&](const std::string& name, std::string networkMode = "bridge") {
            WSLCContainerLauncher launcher("debian:latest", name, {"sleep", "99999"}, {}, std::move(networkMode));
            return launcher.Launch(*m_defaultSession);
        };

        auto createNetwork = [&](const std::string& name, const char* subnet) {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(name.c_str()));
            WSLCNetworkOptions netOpts{};
            netOpts.Name = name.c_str();
            netOpts.Driver = "bridge";
            netOpts.Subnet = subnet;
            VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        };

        // Verifies both ConnectToNetwork and DisconnectFromNetwork reject with the same error for the given network name.
        auto expectBothReject = [&](auto& container, LPCSTR networkName, HRESULT hr, LPCWSTR message) {
            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName;
            VERIFY_ARE_EQUAL(hr, container.Get().ConnectToNetwork(&options));
            ValidateCOMErrorMessage(message);
            VERIFY_ARE_EQUAL(hr, container.Get().DisconnectFromNetwork(networkName));
            ValidateCOMErrorMessage(message);
        };

        // Round-trip: connect to a network, verify via inspect, then disconnect.
        {
            const std::string networkName = "test-connect-disconnect-net";
            createNetwork(networkName, "172.53.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchContainer("test-connect-disconnect-ctr");

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.at(networkName).IPAddress.empty());

            VERIFY_SUCCEEDED(container.Get().DisconnectFromNetwork(networkName.c_str()));

            auto inspectAfter = container.Inspect();
            VERIFY_IS_FALSE(inspectAfter.NetworkSettings.Networks.contains(networkName));
            VERIFY_IS_TRUE(inspectAfter.NetworkSettings.Networks.contains("bridge"));
        }

        // Empty and non-existent network name.
        {
            auto container = launchContainer("test-connect-invalid-name");

            expectBothReject(container, "", E_INVALIDARG, L"Network name cannot be empty.");

            const std::string nonExistentNetwork = "nonexistent-network";
            const auto expectedError =
                std::format(L"Network not found: '{}'", std::wstring(nonExistentNetwork.begin(), nonExistentNetwork.end()));
            expectBothReject(container, nonExistentNetwork.c_str(), WSLC_E_NETWORK_NOT_FOUND, expectedError.c_str());
        }

        // Host and none mode rejection.
        {
            const std::string networkName = "test-connect-mode-net";
            createNetwork(networkName, "172.52.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto expectModeRejection = [&](const std::string& name, std::string mode) {
                auto container = launchContainer(name, std::move(mode));
                expectBothReject(
                    container,
                    networkName.c_str(),
                    E_INVALIDARG,
                    L"Additional networks are not allowed when the primary network mode is 'host' or 'none'.");
            };

            expectModeRejection("test-connect-host-ctr", "host");
            expectModeRejection("test-connect-none-ctr", "none");
        }

        // ContainerIpAddress not supported.
        {
            auto container = launchContainer("test-connect-ip-ctr");

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = "bridge";
            options.ContainerIpAddress = "10.0.0.5";
            VERIFY_ARE_EQUAL(E_NOTIMPL, container.Get().ConnectToNetwork(&options));
            ValidateCOMErrorMessage(L"ContainerIpAddress is not yet supported.");
        }

        // Connect and disconnect from the container's primary network.
        {
            const std::string containerName = "test-connect-primary-ctr";
            auto container = launchContainer(containerName);

            // Connect to primary should fail — already connected.
            WSLCNetworkConnectionOptions options{};
            options.NetworkName = "bridge";
            VERIFY_ARE_EQUAL(E_FAIL, container.Get().ConnectToNetwork(&options));
            // Docker returns the container name in the error, not the ID.
            const auto expectedError = std::format(
                L"endpoint with name {} already exists in network bridge", std::wstring(containerName.begin(), containerName.end()));
            ValidateCOMErrorMessage(expectedError);

            // Disconnect from primary should succeed.
            VERIFY_SUCCEEDED(container.Get().DisconnectFromNetwork("bridge"));

            auto inspect = container.Inspect();
            VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.contains("bridge"));
        }

        // Connect to same secondary network twice.
        {
            const std::string networkName = "test-connect-twice-net";
            const std::string containerName = "test-connect-twice-ctr";
            createNetwork(networkName, "172.51.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchContainer(containerName);

            WSLCNetworkConnectionOptions options{};
            options.NetworkName = networkName.c_str();
            VERIFY_SUCCEEDED(container.Get().ConnectToNetwork(&options));
            VERIFY_ARE_EQUAL(E_FAIL, container.Get().ConnectToNetwork(&options));
            // Docker returns the container name in the error, not the ID.
            const auto expectedError = std::format(
                L"endpoint with name {} already exists in network {}",
                std::wstring(containerName.begin(), containerName.end()),
                std::wstring(networkName.begin(), networkName.end()));
            ValidateCOMErrorMessage(expectedError);
        }
    }

    WSLC_TEST_METHOD(NetworkAliasCreateTest)
    {
        auto createNetwork = [&](const std::string& name, const char* subnet) {
            LOG_IF_FAILED(m_defaultSession->DeleteNetwork(name.c_str()));
            WSLCNetworkOptions netOpts{};
            netOpts.Name = name.c_str();
            netOpts.Driver = "bridge";
            netOpts.Subnet = subnet;
            VERIFY_SUCCEEDED(m_defaultSession->CreateNetwork(&netOpts, nullptr));
        };

        auto launchWithAliases = [&](const std::string& containerName, const std::string& networkMode, const std::vector<std::string>& aliases) {
            WSLCContainerLauncher launcher("debian:latest", containerName, {"sleep", "99999"}, {}, networkMode);
            for (const auto& a : aliases)
            {
                launcher.AddPrimaryNetworkAlias(a);
            }
            return launcher.Launch(*m_defaultSession);
        };

        auto expectError = [&](const std::string& containerName,
                               const std::string& networkMode,
                               const std::vector<std::string>& aliases,
                               HRESULT expectedResult,
                               const std::wstring& expectedErrorMessage) {
            auto result = wil::ResultFromException([&] { launchWithAliases(containerName, networkMode, aliases); });
            VERIFY_ARE_EQUAL(result, expectedResult);
            ValidateCOMErrorMessage(expectedErrorMessage);
        };

        // Single user-defined network + single alias — success, round-trips via inspect.
        {
            const std::string networkName = "alias-net-single";
            createNetwork(networkName, "172.60.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchWithAliases("alias-ctr-single", networkName, {"db"});

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
        }

        // Multiple aliases on a single network — all present.
        {
            const std::string networkName = "alias-net-multi";
            createNetwork(networkName, "172.61.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            auto container = launchWithAliases("alias-ctr-multi", networkName, {"db", "primary", "backup"});

            auto inspect = container.Inspect();
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
            const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "primary") != endpoint.Aliases.end());
            VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "backup") != endpoint.Aliases.end());
        }

        // Alias on 'host' mode — rejected at the IDL layer.
        {
            expectError(
                "alias-ctr-host",
                "host",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on default 'bridge' mode — rejected (aliases require a user-defined network).
        {
            expectError(
                "alias-ctr-bridge",
                "bridge",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on 'none' mode — rejected.
        {
            expectError(
                "alias-ctr-none",
                "none",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Alias on 'container:' mode — rejected.
        {
            WSLCContainerLauncher targetLauncher("debian:latest", "alias-ctr-target", {"sleep", "99999"}, {});
            auto target = targetLauncher.Launch(*m_defaultSession);

            expectError(
                "alias-ctr-container",
                "container:alias-ctr-target",
                {"db"},
                E_INVALIDARG,
                L"Network aliases require a user-defined network. Use --network to specify one.");
        }

        // Empty alias string — rejected.
        {
            const std::string networkName = "alias-net-empty";
            createNetwork(networkName, "172.62.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            expectError("alias-ctr-empty", networkName, {""}, E_INVALIDARG, L"Network alias cannot be empty.");
        }

        // Unknown KVP key on primary settings — rejected with E_NOTIMPL.
        {
            const std::string networkName = "alias-net-unknown";
            createNetwork(networkName, "172.63.0.0/16");
            auto netCleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteNetwork(networkName.c_str())); });

            LPCSTR args[] = {"sleep", "99999"};
            const KeyValuePair settings[] = {{"IPAddress", "10.0.0.5"}};
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = "alias-ctr-unknown";
            options.InitProcessOptions.CommandLine = {.Values = args, .Count = ARRAYSIZE(args)};
            options.ContainerNetwork.NetworkMode = networkName.c_str();
            options.ContainerNetwork.Settings = settings;
            options.ContainerNetwork.SettingsCount = ARRAYSIZE(settings);

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_ARE_EQUAL(E_NOTIMPL, m_defaultSession->CreateContainer(&options, nullptr, &container));
            ValidateCOMErrorMessage(std::format(
                                        L"Endpoint settings are not yet supported (network '{}').",
                                        std::wstring(networkName.begin(), networkName.end()))
                                        .c_str());
        }
    }

    WSLC_TEST_METHOD(ContainerNetworkModeHappyPathTest)
    {
        // Start container A on the default (bridged) network, then start container B sharing A's
        // network namespace. Verify the inspect round-trip returns the canonical container mode.
        const std::string containerAName = "test-container-mode-a";
        const std::string containerBName = "test-container-mode-b";

        WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
        auto containerA = launcherA.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);

        const std::string containerAId = containerA.Id();

        WSLCContainerLauncher launcherB("debian:latest", containerBName, {"sleep", "99999"}, {}, "container:" + containerAName);

        auto containerB = launcherB.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerB.State(), WslcContainerStateRunning);

        // Inspect B: NetworkMode must be "container:<A's canonical 64-char id>".
        const std::string expectedNetworkMode = "container:" + containerAId;
        VERIFY_ARE_EQUAL(containerB.Inspect().HostConfig.NetworkMode, expectedNetworkMode);
    }

    WSLC_TEST_METHOD(ContainerNetworkModeMissingTargetRejectedTest)
    {
        // Container mode with an empty target name must be rejected before any Docker call.
        WSLCContainerLauncher launcher("debian:latest", "test-container-mode-no-target", {"sleep", "99999"}, {}, "container:");

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(L"Target container name is required for container network mode.");
    }

    WSLC_TEST_METHOD(ContainerNetworkModeTargetNotFoundTest)
    {
        // Container mode with a nonexistent target must return WSLC_E_CONTAINER_NOT_FOUND
        // with a localized message naming the target.
        const std::string targetName = "does-not-exist-container-target";

        WSLCContainerLauncher launcher("debian:latest", "test-container-mode-notfound", {"sleep", "99999"}, {}, "container:" + targetName);

        auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_NOT_FOUND, retVal.first);
        ValidateCOMErrorMessage(std::format(L"Target container '{}' not found.", targetName));
    }

    WSLC_TEST_METHOD(ContainerNetworkModePortsRejectedTest)
    {
        // Container mode does not support port mappings — ports belong to the target container.
        const std::string containerAName = "test-container-mode-ports-a";

        WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
        auto containerA = launcherA.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);

        WSLCContainerLauncher launcherB("debian:latest", "test-container-mode-ports-b", {"sleep", "99999"}, {}, "container:" + containerAName);
        launcherB.AddPort(8080, 80, AF_INET);

        auto retVal = launcherB.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(E_INVALIDARG, retVal.first);
        ValidateCOMErrorMessage(
            L"Port mappings are not supported with container network mode; ports are owned by the target container.");
    }

    WSLC_TEST_METHOD(ContainerNetworkModeInspectRoundTripTest)
    {
        // Verify that after a session reset (service restart), Inspect() on a recovered
        // container-mode container still returns the correct "container:<id>" NetworkMode.
        const std::string containerAName = "test-container-mode-rt-a";
        const std::string containerBName = "test-container-mode-rt-b";

        std::string containerAId;

        {
            WSLCContainerLauncher launcherA("debian:latest", containerAName, {"sleep", "99999"}, {});
            auto containerA = launcherA.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(containerA.State(), WslcContainerStateRunning);
            containerAId = containerA.Id();
            containerA.SetDeleteOnClose(false);

            WSLCContainerLauncher launcherB("debian:latest", containerBName, {"sleep", "99999"}, {}, "container:" + containerAName);
            auto containerB = launcherB.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(containerB.State(), WslcContainerStateCreated);
            containerB.SetDeleteOnClose(false);
        }

        // Simulate service restart — Open() path reconstructs container from Docker state.
        ResetTestSession();

        auto recoveredContainerA = OpenContainer(m_defaultSession.get(), containerAName);
        auto recoveredContainerB = OpenContainer(m_defaultSession.get(), containerBName);
        VERIFY_ARE_EQUAL(recoveredContainerB.State(), WslcContainerStateCreated);

        const std::string expectedNetworkMode = "container:" + containerAId;
        VERIFY_ARE_EQUAL(recoveredContainerB.Inspect().HostConfig.NetworkMode, expectedNetworkMode);
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
        auto expectMounts = [&](const auto& actualMounts, const std::vector<std::tuple<std::string, std::string, bool>>& expectedMounts) {
            VERIFY_ARE_EQUAL(actualMounts.size(), expectedMounts.size());

            for (const auto& [expectedDest, expectedType, expectedReadWrite] : expectedMounts)
            {
                auto it = std::ranges::find_if(actualMounts, [&](const auto& mount) { return mount.Destination == expectedDest; });
                if (it == actualMounts.end())
                {
                    LogError("Expected mount destination not found: %hs", expectedDest.c_str());
                    VERIFY_FAIL();
                }

                VERIFY_IS_FALSE(it->Type.empty());
                VERIFY_ARE_EQUAL(it->Type, expectedType);

                if (expectedType != "tmpfs")
                {
                    VERIFY_IS_FALSE(it->Source.empty());
                }
                VERIFY_ARE_EQUAL(it->ReadWrite, expectedReadWrite);
            }
        };

        // Test a running container with port mappings and volumes.
        {
            auto testFolder = std::filesystem::current_path() / "test-inspect-volume";
            auto testFolderReadOnly = std::filesystem::current_path() / "test-inspect-volume-ro";

            std::filesystem::create_directories(testFolder);
            std::filesystem::create_directories(testFolderReadOnly);

            auto cleanup = wil::scope_exit([&]() {
                std::error_code ec;
                std::filesystem::remove_all(testFolder, ec);
                std::filesystem::remove_all(testFolderReadOnly, ec);
            });

            WSLCContainerLauncher launcher("debian:latest", "test-container-inspect", {"sleep", "99999"}, {}, "bridge");

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1235, 8000, AF_INET);
            launcher.AddPort(1236, 8001, AF_INET);
            launcher.AddVolume(testFolder.wstring(), "/test-volume", false);
            launcher.AddVolume(testFolderReadOnly.wstring(), "/test-volume-ro", true);
            launcher.AddTmpfs("/mnt/wslc-tmpfs-inspect", "");

            auto container = launcher.Launch(*m_defaultSession);

            // Validate that inspect fails with a null pointer.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), container.Get().Inspect(nullptr));

            auto details = container.Inspect();

            // Verify basic container metadata.
            VERIFY_IS_FALSE(details.Id.empty());
            VERIFY_ARE_EQUAL(details.Name, "test-container-inspect");
            VERIFY_ARE_EQUAL(details.Image, "debian:latest");
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
                {{"/test-volume", "bind", true}, {"/test-volume-ro", "bind", false}, {"/mnt/wslc-tmpfs-inspect", "tmpfs", true}});

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
            VERIFY_ARE_EQUAL(details.Name, "test-container-inspect-exited");
            VERIFY_ARE_EQUAL(details.Image, "debian:latest");
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

    void RunPortMappingsTest(IWSLCSession& session, const std::string& containerNetworkType, bool virtionet)
    {
        WEX::Logging::Log::Comment(
            std::format(L"Container network type: {}", wsl::shared::string::MultiByteToWide(containerNetworkType)).c_str());

        auto expectBoundPorts = [&](RunningWSLCContainer& Container, const std::vector<std::string>& expectedBoundPorts) {
            auto ports = Container.Inspect().Ports;

            std::vector<std::string> boundPorts;
            for (const auto& e : ports)
            {
                boundPorts.emplace_back(e.first);
            }

            if (!std::ranges::equal(boundPorts, expectedBoundPorts))
            {
                LogError(
                    "Port bindings do not match expected values. Expected: [%hs], Actual: [%hs]",
                    wsl::shared::string::Join(expectedBoundPorts, ',').c_str(),
                    wsl::shared::string::Join(boundPorts, ',').c_str());

                VERIFY_FAIL();
            }
        };

        // Test a simple port mapping.
        {
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports", {"python3", "-m", "http.server", "--bind", "::"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET6, IPPROTO_TCP, "::1");

            auto container = launcher.Launch(session);
            auto initProcess = container.GetInitProcess();

            // Wait for the container bind() to be completed.
            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

            expectBoundPorts(container, {"8000/tcp"});

            ExpectHttpResponse(L"http://127.0.0.1:1234", 200);

            ExpectHttpResponse(L"http://[::1]:1234", 200);

            // Verify that ListContainers returns the port data for a running container.
            {
                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                // Find the container ID for "test-ports"
                std::string testPortsId;
                for (const auto& entry : containers)
                {
                    if (std::string(entry.Name) == "test-ports")
                    {
                        testPortsId = entry.Id;
                        break;
                    }
                }
                VERIFY_IS_FALSE(testPortsId.empty());

                // Filter ports for this container
                std::vector<WSLCPortMapping> containerPorts;
                for (const auto& port : ports)
                {
                    if (testPortsId == port.Id)
                    {
                        containerPorts.push_back(port.PortMapping);
                    }
                }

                VERIFY_ARE_EQUAL(2, containerPorts.size());
                VERIFY_ARE_EQUAL(1234, containerPorts[0].HostPort);
                VERIFY_ARE_EQUAL(8000, containerPorts[0].ContainerPort);
                VERIFY_ARE_EQUAL(AF_INET, containerPorts[0].Family);
                VERIFY_ARE_EQUAL(1234, containerPorts[1].HostPort);
                VERIFY_ARE_EQUAL(8000, containerPorts[1].ContainerPort);
                VERIFY_ARE_EQUAL(AF_INET6, containerPorts[1].Family);
                VERIFY_ARE_EQUAL(IPPROTO_TCP, containerPorts[0].Protocol);
                VERIFY_ARE_EQUAL(IPPROTO_TCP, containerPorts[1].Protocol);
            }

            // Verify that a created (not yet started) container returns no ports.
            {
                WSLCContainerLauncher createdLauncher("debian:latest", "test-ports-created", {"echo", "OK"}, {}, containerNetworkType);
                createdLauncher.AddPort(1235, 8000, AF_INET);

                auto createdContainer = createdLauncher.Create(session);

                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                std::string createdId = createdContainer.Id();
                for (const auto& port : ports)
                {
                    VERIFY_ARE_NOT_EQUAL(createdId, std::string(port.Id));
                }

                VERIFY_SUCCEEDED(createdContainer.Get().Delete(WSLCDeleteFlagsNone));
                createdContainer.Reset();
            }

            // Validate that the port cannot be reused while the container is running.
            WSLCContainerLauncher subLauncher(
                "python:3.12-alpine", "test-ports-2", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            subLauncher.AddPort(1234, 8000, AF_INET);

            auto [hresult, newContainer] = subLauncher.LaunchNoThrow(session);
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(WSAEADDRINUSE));

            // Verify that a stopped container returns no ports.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            {
                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(
                    nullptr, &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

                std::string stoppedId = container.Id();
                for (const auto& port : ports)
                {
                    VERIFY_ARE_NOT_EQUAL(stoppedId, std::string(port.Id));
                }
            }

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            container.Reset(); // TODO: Re-think container lifetime management.

            // Validate that the port can be reused now that the container is stopped.
            {
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-3", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

                launcher.AddPort(1234, 8000, AF_INET);

                auto container = launcher.Launch(session);
                auto initProcess = container.GetInitProcess();

                // Wait for the container bind() to be completed.
                WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on 0.0.0.0 port 8000");

                expectBoundPorts(container, {"8000/tcp"});
                ExpectHttpResponse(L"http://127.0.0.1:1234", 200);

                VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
                VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
                container.Reset(); // TODO: Re-think container lifetime management.
            }
        }

        // Validate that the same host port can't be bound twice in the same Create() call.
        {
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1234, 8000, AF_INET);

            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));
        }

        auto bindSocket = [](auto port) {
            wil::unique_socket socket(WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            VERIFY_ARE_NOT_EQUAL(bind(socket.get(), (sockaddr*)&address, sizeof(address)), SOCKET_ERROR);
            return socket;
        };

        // Validate that Create() fails if the port is already bound.
        {
            auto boundSocket = bindSocket(1235);
            WSLCContainerLauncher launcher(
                "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);

            launcher.AddPort(1235, 8000, AF_INET);
            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));

            // Validate that Create() correctly cleans up bound ports after a port fails to map
            {
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);
                launcher.AddPort(1236, 8000, AF_INET); // Should succeed
                launcher.AddPort(1235, 8000, AF_INET); // Should fail.

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEADDRINUSE));

                // Validate that port 1236 is still available (was cleaned up after failure).
                VERIFY_IS_TRUE(!!bindSocket(1236));
            }
        }

        // Validate error paths
        {
            // Invalid IP address
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "invalid-ip");

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, E_INVALIDARG);
                ValidateCOMErrorMessage(L"Invalid IP address 'invalid-ip'");
            }

            // Invalid protocol
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, 1);

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, E_INVALIDARG);
            }

            // Invalid address family (launched manually because AddPort() throws on unsupported family).
            {
                WSLCPortMapping port{};
                strcpy_s(port.BindingAddress, "127.0.0.1");
                port.HostPort = 1234;
                port.ContainerPort = 1234;
                port.Protocol = IPPROTO_TCP;
                port.Family = AF_UNIX; // Unsupported

                WSLCContainerOptions options{};
                options.Image = "python:3.12-alpine";
                options.Ports = &port;
                options.PortsCount = 1;
                options.ContainerNetwork.NetworkMode = containerNetworkType.c_str();

                wil::com_ptr<IWSLCContainer> container;
                VERIFY_ARE_EQUAL(session.CreateContainer(&options, nullptr, &container), E_INVALIDARG);
            }

            if (virtionet)
            {
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_UDP);

                    VERIFY_SUCCEEDED(launcher.LaunchNoThrow(session).first);
                }

                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0");

                    VERIFY_SUCCEEDED(launcher.LaunchNoThrow(session).first);
                }
            }
            else
            {
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_UDP);

                    VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
                }

                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0");

                    VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
                }
            }
        }
    }

    auto SetupPortMappingsTest(WSLCNetworkingMode networkingMode, WSLCFeatureFlags featureFlags = WslcFeatureFlagsNone)
    {
        auto settings = GetDefaultSessionSettings(L"networking-session", true, networkingMode);
        settings.FeatureFlags = featureFlags;

        auto createNewSession = settings.NetworkingMode != m_defaultSessionSettings.NetworkingMode ||
                                settings.FeatureFlags != m_defaultSessionSettings.FeatureFlags;
        auto restore = createNewSession ? std::optional{ResetTestSession()} : std::nullopt;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        return std::make_pair(std::move(restore), std::move(session));
    }

    WSLC_TEST_METHOD(PortMappingsNat)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeNAT);

        RunPortMappingsTest(*session, "bridge", false);
        RunPortMappingsTest(*session, "host", false);
    }

    WSLC_TEST_METHOD(PortMappingsConsomme)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme);

        RunPortMappingsTest(*session, "bridge", true);
        RunPortMappingsTest(*session, "host", true);
    }

    WSLC_TEST_METHOD(PortMappingsConsommeWslRelay)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme, WslcFeatureFlagsPortRelayWslRelay);

        RunPortMappingsTest(*session, "bridge", false);
        RunPortMappingsTest(*session, "host", false);
    }

    WSLC_TEST_METHOD(PortMappingsAdvanced)
    {
        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeConsomme);

        auto hostIp = GetHostAdapterIpv4();
        std::optional<std::string> hostIpNarrow;
        if (hostIp.has_value())
        {
            hostIpNarrow = wsl::shared::string::WideToMultiByte(hostIp.value());
        }

        struct PortMapping
        {
            uint16_t HostPort;
            uint16_t ContainerPort;
            int Family;
            int Protocol = IPPROTO_TCP;
            std::optional<std::string> BindingAddress;
        };

        auto runCustomBindingTests = [&](const std::string& containerNetworkType) {
            LogInfo("Container network type: %hs", containerNetworkType.c_str());

            auto createTcpContainer = [&](const std::vector<PortMapping>& ports) {
                static int containerIndex = 0;
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine",
                    std::format("test-ports-custom-{}", containerIndex++),
                    {"python3", "-m", "http.server", "--bind", "::"},
                    {"PYTHONUNBUFFERED=1"},
                    containerNetworkType);

                for (const auto& port : ports)
                {
                    launcher.AddPort(port.HostPort, port.ContainerPort, port.Family, port.Protocol, port.BindingAddress);
                }

                auto container = launcher.Launch(*session);
                WaitForOutput(container.GetInitProcess().GetStdHandle(1), "Serving HTTP on");
                return container;
            };

            auto validateInspectPortBinding = [&](auto& container,
                                                  uint16_t containerPort,
                                                  int protocol,
                                                  const std::string& expectedHostIp,
                                                  std::optional<uint16_t> expectedHostPort) -> std::string {
                auto inspectData = container.Inspect();

                auto portKey = std::format("{}/{}", containerPort, protocol == IPPROTO_UDP ? "udp" : "tcp");
                VERIFY_IS_TRUE(inspectData.Ports.contains(portKey));

                auto& bindings = inspectData.Ports[portKey];
                VERIFY_ARE_EQUAL(1u, bindings.size());
                VERIFY_ARE_EQUAL(expectedHostIp, bindings[0].HostIp);

                if (expectedHostPort.has_value())
                {
                    VERIFY_ARE_EQUAL(std::to_string(expectedHostPort.value()), bindings[0].HostPort);
                }
                else
                {
                    VERIFY_IS_TRUE(std::stoi(bindings[0].HostPort) > 0);
                }

                return bindings[0].HostPort;
            };

            // Explicit localhost (127.0.0.1) binding.
            {
                auto container = createTcpContainer({{1260, 8000, AF_INET, IPPROTO_TCP, "127.0.0.1"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "127.0.0.1", 1260);
                ExpectHttpResponse(L"http://127.0.0.1:1260", 200);
            }

            // 0.0.0.0 (all interfaces) binding.
            {
                auto container = createTcpContainer({{1261, 8000, AF_INET, IPPROTO_TCP, "0.0.0.0"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "0.0.0.0", 1261);

                // Verify reachable via loopback.
                ExpectHttpResponse(L"http://127.0.0.1:1261", 200);

                // Verify reachable via host adapter IP to confirm wildcard semantics.
                if (hostIp.has_value())
                {
                    auto url = std::format(L"http://{}:1261", hostIp.value());
                    ExpectHttpResponse(url.c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP verification: no suitable IPv4 adapter found");
                }
            }

            // Main host adapter's IPv4 address binding.
            {
                if (hostIp.has_value())
                {
                    auto container = createTcpContainer({{1262, 8000, AF_INET, IPPROTO_TCP, hostIpNarrow.value()}});
                    validateInspectPortBinding(container, 8000, IPPROTO_TCP, hostIpNarrow.value(), 1262);

                    auto url = std::format(L"http://{}:1262", hostIp.value());
                    ExpectHttpResponse(url.c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP binding test: no suitable IPv4 adapter found");
                }
            }

            // Anonymous bind on localhost (ephemeral host port).
            {
                auto container = createTcpContainer({{WSLC_EPHEMERAL_PORT, 8000, AF_INET, IPPROTO_TCP, "127.0.0.1"}});
                auto hostPort = validateInspectPortBinding(container, 8000, IPPROTO_TCP, "127.0.0.1", std::nullopt);

                ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", hostPort).c_str(), 200);
            }

            // Anonymous bind on host ip (ephemeral host port).
            {
                if (hostIp.has_value())
                {
                    auto container = createTcpContainer({{WSLC_EPHEMERAL_PORT, 8000, AF_INET, IPPROTO_TCP, hostIpNarrow.value()}});
                    auto hostPort = validateInspectPortBinding(container, 8000, IPPROTO_TCP, hostIpNarrow.value(), std::nullopt);

                    ExpectHttpResponse(std::format(L"http://{}:{}", hostIp.value(), hostPort).c_str(), 200);
                }
                else
                {
                    LogInfo("Skipping host adapter IP binding test: no suitable IPv4 adapter found");
                }
            }

            // IPv6 loopback (::1) binding.
            {
                auto container = createTcpContainer({{1263, 8000, AF_INET6, IPPROTO_TCP, "::1"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "::1", 1263);
                ExpectHttpResponse(L"http://[::1]:1263", 200);
            }

            // IPv6 wildcard (::) binding.
            {
                auto container = createTcpContainer({{1264, 8000, AF_INET6, IPPROTO_TCP, "::"}});
                validateInspectPortBinding(container, 8000, IPPROTO_TCP, "::", 1264);
                ExpectHttpResponse(L"http://[::1]:1264", 200);
            }

            // UDP port mapping with a Python echo server.
            {
                // Inline Python UDP echo server: receives a datagram and sends it back uppercased.
                static constexpr auto c_udpEchoScript =
                    "import socket,sys;"
                    "s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM);"
                    "s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,0);"
                    "s.bind(('::',9000));"
                    "print('UDP listening',flush=True);"
                    "data,addr=s.recvfrom(1024);"
                    "s.sendto(data.upper(),addr)";

                static int udpContainerIndex = 0;
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine",
                    std::format("test-ports-custom-udp-{}", udpContainerIndex++),
                    {"python3", "-c", c_udpEchoScript},
                    {"PYTHONUNBUFFERED=1"},
                    containerNetworkType);

                launcher.AddPort(1265, 9000, AF_INET, IPPROTO_UDP, "127.0.0.1");

                auto container = launcher.Launch(*session);
                WaitForOutput(container.GetInitProcess().GetStdHandle(1), "UDP listening");
                validateInspectPortBinding(container, 9000, IPPROTO_UDP, "127.0.0.1", 1265);

                WSLCE2ETests::SendUdpAndReceive(1265, "hello", "HELLO");
            }

            // Validate that trying to bind an address that the host doesn't have fails:
            {
                // Malformed address string.
                {
                    WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                    launcher.AddPort(1265, 8000, AF_INET, IPPROTO_TCP, "1.1.1.1");

                    auto container = launcher.Create(*session);
                    validateInspectPortBinding(container, 8000, IPPROTO_TCP, "1.1.1.1", 1265);

                    VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr, nullptr), HRESULT_FROM_WIN32(WSAEADDRNOTAVAIL));
                    ValidateCOMErrorMessage(
                        L"Failed to map port '1.1.1.1:1265/tcp', The requested address is not valid in its context. ");
                }
            }
        };

        runCustomBindingTests("bridge");
        runCustomBindingTests("host");
    }

    TEST_METHOD(PortMappingsNone)
    {
        // Validate that trying to map ports without network fails.
        WSLCContainerLauncher launcher(
            "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, "none");

        launcher.AddPort(1234, 8000, AF_INET);

        VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*m_defaultSession).first, E_INVALIDARG);
    }

    WSLC_TEST_METHOD(PublishAllExposedPorts)
    {
        // Build a test image with EXPOSE directives.
        auto contextDir = std::filesystem::current_path() / "build-context-publish-all";
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LOG_IF_FAILED(DeleteImageNoThrow("wslc-test-publish-all:latest", WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
        });

        // TODO: Add test coverage for exposed UDP ports once supported.
        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM python:3.12-alpine\n";
            dockerfile << "EXPOSE 8080/tcp\n";
            dockerfile << "EXPOSE 9090/tcp\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-publish-all:latest"));

        // Run a container with --publish-all using the API.
        {
            WSLCContainerLauncher launcher(
                "wslc-test-publish-all:latest",
                "test-publish-all",
                {"python3", "-m", "http.server", "--bind", "::", "8080"},
                {"PYTHONUNBUFFERED=1"},
                "bridge");

            launcher.SetContainerFlags(WSLCContainerFlagsPublishAll);

            auto container = launcher.Launch(*m_defaultSession);
            auto initProcess = container.GetInitProcess();

            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on");

            // Verify the container has port mappings for the exposed ports.
            auto inspectData = container.Inspect();
            VERIFY_IS_TRUE(inspectData.Ports.contains("8080/tcp"));
            VERIFY_IS_TRUE(inspectData.Ports.contains("9090/tcp"));

            // Verify we can connect to the 8080 exposed port from the host.
            auto portBindings8080 = inspectData.Ports["8080/tcp"];
            VERIFY_ARE_EQUAL(1u, portBindings8080.size());
            auto hostPort8080 = std::stoi(portBindings8080[0].HostPort);
            VERIFY_IS_TRUE(hostPort8080 > 0);

            ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", hostPort8080).c_str(), 200);

            // Verify the second exposed port got a mapping too.
            auto portBindings9090 = inspectData.Ports["9090/tcp"];
            VERIFY_ARE_EQUAL(1u, portBindings9090.size());
            auto hostPort9090 = std::stoi(portBindings9090[0].HostPort);
            VERIFY_IS_TRUE(hostPort9090 > 0);

            // The two host ports must be different.
            VERIFY_ARE_NOT_EQUAL(hostPort8080, hostPort9090);
        }
    }

    WSLC_TEST_METHOD(PublishAllImageNotFound)
    {
        // Verify that using PublishAll with a nonexistent image still returns IMAGE_NOT_FOUND.
        WSLCContainerLauncher launcher("invalid-image-name:nonexistent", "dummy-publish-all", {"/bin/cat"}, {}, "bridge");
        launcher.SetContainerFlags(WSLCContainerFlagsPublishAll);

        auto [hresult, container] = launcher.LaunchNoThrow(*m_defaultSession);
        VERIFY_ARE_EQUAL(hresult, WSLC_E_IMAGE_NOT_FOUND);
    }

    void ValidateContainerVolumes(bool enableVirtioFs)
    {
        auto restore = ResetTestSession();
        auto hostFolder = std::filesystem::current_path() / "test-volume";
        auto hostFolderReadOnly = std::filesystem::current_path() / "test-volume-ro";

        std::filesystem::create_directories(hostFolder);
        std::filesystem::create_directories(hostFolderReadOnly);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(hostFolderReadOnly, ec);
        });

        auto settings = GetDefaultSessionSettings(L"volumes-tests", true);
        WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs, enableVirtioFs);

        auto session = CreateSession(settings);

        // Validate both folders exist in the container and that the readonly one cannot be written to.
        std::string containerName = "test-container";
        std::string containerPath = "/volume";
        std::string containerReadOnlyPath = "/volume-ro";

        // Container init script to validate volumes are mounted correctly.
        const std::string script =
            "set -e; "

            // Test that volumes are available in the container
            "test -d " +
            containerPath +
            "; "
            "test -d " +
            containerReadOnlyPath +
            "; "

            // Test that the container cannot write to the read-only volume
            "if touch " +
            containerReadOnlyPath +
            "/.ro-test 2>/dev/null;"
            "then echo 'FAILED'; "
            "else echo 'OK'; "
            "fi ";

        WSLCContainerLauncher launcher("debian:latest", containerName, {"/bin/sh", "-c", script});
        launcher.AddVolume(hostFolder.wstring(), containerPath, false);
        launcher.AddVolume(hostFolderReadOnly.wstring(), containerReadOnlyPath, true);

        {
            auto container = launcher.Launch(*session);
            auto process = container.GetInitProcess();
            ValidateProcessOutput(process, {{1, "OK\n"}});

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Validate that the volumes are not mounted after container exits.
        ExpectMount(session.get(), std::format("/mnt/wslc/{}/volumes/{}", containerName, 0), {});
        ExpectMount(session.get(), std::format("/mnt/wslc/{}/volumes/{}", containerName, 1), {});
    }

    TEST_METHOD(ContainerVolume)
    {
        ValidateContainerVolumes(false);
    }

    TEST_METHOD(ContainerVolumeVirtioFs)
    {
        ValidateContainerVolumes(true);
    }

    WSLC_TEST_METHOD(ContainerVolumesAdvanced)
    {
        auto hostFolder = std::filesystem::weakly_canonical(std::filesystem::current_path() / "test-volume");
        auto symlinkFolder = std::filesystem::weakly_canonical(std::filesystem::current_path() / "test-volume-symlink");
        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(symlinkFolder, ec);
        });

        VERIFY_IS_TRUE((std::ofstream(hostFolder / "file.txt") << "OK").good());
        std::filesystem::create_symlink("file.txt", hostFolder / "symlink");

        // N.B. std::filesystem::create_symlink doesn't correctly handle folder symlinks.
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateSymbolicLink(symlinkFolder.c_str(), hostFolder.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY));

        // Validate a simple folder mount.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-1", {"cat", "/volume/file.txt"});
            launcher.AddVolume(hostFolder.wstring(), "/volume", false);

            ValidateContainerOutput(launcher, {{1, "OK"}});
        }

        // Validate that files can be mounted too.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-2", {"cat", "/volume"});
            launcher.AddVolume((hostFolder / "file.txt").wstring(), "/volume", false);
            ValidateContainerOutput(launcher, {{1, "OK"}});
        }

        // Validate that file symlinks work as expected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-3", {"cat", "/volume"});
            launcher.AddVolume((hostFolder / "symlink").wstring(), "/volume", false);
            ValidateContainerOutput(launcher, {{1, "OK"}});
        }

        // Validate that folder symlinks work as expected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-4", {"cat", "/volume/file.txt", "/volume/symlink"});
            launcher.AddVolume(symlinkFolder.wstring(), "/volume", false);

            ValidateContainerOutput(launcher, {{1, "OKOK"}});
        }

        // Validate that folders are created if they don't exist.
        {
            {
                WSLCContainerLauncher launcher(
                    "debian:latest", "test-volumes-5", {"/bin/sh", "-c", "echo created > /volume/new-file"});
                launcher.AddVolume((hostFolder / "should-be-created").wstring(), "/volume", false);
                ValidateContainerOutput(launcher, {{1, ""}});
            }

            VERIFY_ARE_EQUAL(ReadFileContent(hostFolder / "should-be-created" / "new-file"), L"created\n");
        }

        // Validate that relative paths are rejected
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-6", {});
            launcher.AddVolume(L"relative-path", "/volume", false);

            auto [result, container] = launcher.LaunchNoThrow(*m_defaultSession);

            VERIFY_ARE_EQUAL(result, E_INVALIDARG);
        }

        // Validate that invalid paths are rejected
        {
            WSLCContainerLauncher launcher("debian:latest", "test-volumes-7", {});
            launcher.AddVolume(L":", "/volume", false);

            auto [result, container] = launcher.LaunchNoThrow(*m_defaultSession);

            VERIFY_ARE_EQUAL(result, E_INVALIDARG);
        }

        // Validate that access denied errors are propagated when the host volume folder can't be created.
        {
            SetPathAccess(hostFolder, FILE_GENERIC_WRITE, DENY_ACCESS);

            auto restoreAccess =
                wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { SetPathAccess(hostFolder, FILE_GENERIC_WRITE, GRANT_ACCESS); });

            WSLCContainerLauncher launcher("debian:latest", "test-volumes-8", {"echo", "OK"});
            launcher.AddVolume((hostFolder / "subfolder").wstring(), "/volume", false);

            auto [result, container] = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(result, E_ACCESSDENIED);

            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            VerifyPatternMatch(
                wsl::shared::string::WideToMultiByte(comError->Message.get()),
                "Failed to create volume '*test-volume\\subfolder': Access is denied. ");
        }

        // Validate that files mounts are correctly recovered when a container is loaded from storage
        {
            auto validateInspect = [&](auto& container) {
                auto inspect = container.Inspect();
                VERIFY_ARE_EQUAL(inspect.Mounts.size(), 1);
                VERIFY_ARE_EQUAL(inspect.Mounts[0].Destination, "/volume");
                VERIFY_ARE_EQUAL(inspect.Mounts[0].Source, (hostFolder / "file.txt").string());
                VERIFY_ARE_EQUAL(inspect.Mounts[0].ReadWrite, true);
                VERIFY_ARE_EQUAL(inspect.Mounts[0].Type, "bind");
            };

            WSLCContainerLauncher launcher("debian:latest", "test-volumes-8", {"/bin/cat", "/volume"});
            launcher.AddVolume((hostFolder / "file.txt").wstring(), "/volume", false);
            auto container = launcher.Create(*m_defaultSession);
            validateInspect(container);

            ResetTestSession();
            container.SetDeleteOnClose(false);

            auto openedContainer = OpenContainer(m_defaultSession.get(), "test-volumes-8");
            VERIFY_SUCCEEDED(openedContainer.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));
            validateInspect(openedContainer);

            ValidateContainerOutput(openedContainer, {{1, "OK"}});
        }
    }

    void ValidateContainerVolumeUnmountAllFoldersOnError(bool enableVirtioFs)
    {
        auto hostFolder = std::filesystem::current_path() / "test-volume";
        auto storage = std::filesystem::current_path() / "storage";

        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
            std::filesystem::remove_all(storage, ec);
        });

        auto settings = GetDefaultSessionSettings(L"unmount-test");
        WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs, enableVirtioFs);

        // Reuse the default session if possible.
        auto createNewSession = enableVirtioFs != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Create a container with a simple command.
        WSLCContainerLauncher launcher("debian:latest", "test-container", {"/bin/echo", "OK"});
        launcher.AddVolume(hostFolder.wstring(), "/volume", false);

        // Add a volume with an invalid (non-existing) host path
        launcher.AddVolume(L"does-not-exist", "/volume-invalid", false);

        auto [result, container] = launcher.LaunchNoThrow(*session);
        VERIFY_FAILED(result);

        // Verify that the first volume was mounted before the error occurred, then unmounted after failure.
        ExpectMount(session.get(), "/mnt/wslc/test-container/volumes/0", {});
    }

    TEST_METHOD(ContainerVolumeUnmountAllFoldersOnError)
    {
        ValidateContainerVolumeUnmountAllFoldersOnError(false);
    }

    TEST_METHOD(ContainerVolumeUnmountAllFoldersOnErrorVirtioFs)
    {
        ValidateContainerVolumeUnmountAllFoldersOnError(true);
    }

    TEST_METHOD(LineBasedReader)
    {
        auto runTest = [](bool Crlf, const std::string& Data, const std::vector<std::string>& ExpectedLines) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> lines;
            auto onData = [&](const gsl::span<char>& data) { lines.emplace_back(data.data(), data.size()); };

            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::LineBasedReadHandle>(std::move(readPipe), std::move(onData), Crlf));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::io::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < lines.size(); i++)
            {
                if (i >= ExpectedLines.size())
                {
                    LogError(
                        "Input: '%hs'. Line %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedLines[i] != lines[i])
                {
                    LogError(
                        "Input: '%hs'. Line %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedLines[i]).c_str(),
                        EscapeString(lines[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedLines.size() != lines.size())
            {
                LogError(
                    "Input: '%hs', Number of lines do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedLines.size(),
                    lines.size());
                VERIFY_FAIL();
            }
        };

        runTest(false, "foo\nbar", {"foo", "bar"});
        runTest(false, "foo", {"foo"});
        runTest(false, "\n", {});
        runTest(false, "\n\n", {});
        runTest(false, "\n\r\n", {"\r"});
        runTest(false, "\n\nfoo\nbar", {"foo", "bar"});
        runTest(false, "foo\r\nbar", {"foo\r", "bar"});
        runTest(true, "foo\nbar", {"foo\nbar"});
        runTest(true, "foo\r\nbar", {"foo", "bar"});
        runTest(true, "foo\rbar\nbaz", {"foo\rbar\nbaz"});
        runTest(true, "\r", {"\r"});
    }

    TEST_METHOD(HTTPChunkReader)
    {
        auto runTest = [](const std::string& Data, const std::vector<std::string>& ExpectedChunk) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::vector<std::string> chunks;
            auto onData = [&](const gsl::span<char>& data) { chunks.emplace_back(data.data(), data.size()); };

            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::HTTPChunkBasedReadHandle>(std::move(readPipe), std::move(onData)));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::io::WriteHandle>(std::move(writePipe), buffer));

            io.Run({});

            for (size_t i = 0; i < ExpectedChunk.size(); i++)
            {
                if (i >= chunks.size())
                {
                    LogError(
                        "Input: '%hs': Chunk %zu is missing. Expected: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedChunk[i] != chunks[i])
                {
                    LogError(

                        "Input: '%hs': Chunk %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(Data).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str(),
                        EscapeString(chunks[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedChunk.size() != chunks.size())
            {
                LogError(
                    "Input: '%hs', Number of chunks do not match. Expected: %zu, Actual: %zu",
                    EscapeString(Data).c_str(),
                    ExpectedChunk.size(),
                    chunks.size());
                VERIFY_FAIL();
            }
        };

        runTest("3\r\nfoo\r\n3\r\nbar", {"foo", "bar"});
        runTest("3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n", {"foo", "bar"});
        runTest("1\r\na\r\n\r\n", {"a"});

        runTest("c\r\nlf\nin\r\nchunk\r\n3\r\nEOF", {"lf\nin\r\nchunk", "EOF"});
        runTest("15\r\n\r\nchunkstartingwithlf\r\n3\r\nEOF", {"\r\nchunkstartingwithlf", "EOF"});

        // Validate that invalid chunk sizes fail
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("Invalid\r\nInvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4nolf", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("12\nyeseighteenletters", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4invalid\nnocr", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid", {}); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest("4\rinvalid\n", {}); }), E_INVALIDARG);
    }

    TEST_METHOD(HTTPChunkReaderSplitReads)
    {
        auto runTest = [](const std::vector<std::string>& Data, const std::vector<std::string>& ExpectedChunk) {
            std::vector<std::string> chunks;
            auto onData = [&](const gsl::span<char>& data) { chunks.emplace_back(data.data(), data.size()); };

            auto reader = std::make_unique<wsl::windows::common::io::HTTPChunkBasedReadHandle>(
                wsl::windows::common::io::HandleWrapper{nullptr}, std::move(onData));

            std::string allData;
            for (const auto& datum : Data)
            {
                size_t currentSize = allData.size();
                allData.append(datum);
                reader->OnRead(gsl::span<char>{&allData[currentSize], datum.size()});
            }

            // Final 0 byte read
            reader->OnRead(gsl::span<char>{nullptr, static_cast<size_t>(0)});

            for (size_t i = 0; i < ExpectedChunk.size(); i++)
            {
                if (i >= chunks.size())
                {
                    LogError(
                        "Input: '%hs': Chunk %zu is missing. Expected: '%hs'",
                        EscapeString(allData).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str());
                    VERIFY_FAIL();
                }
                else if (ExpectedChunk[i] != chunks[i])
                {
                    LogError(

                        "Input: '%hs': Chunk %zu does not match expected value. Expected: '%hs', Actual: '%hs'",
                        EscapeString(allData).c_str(),
                        i,
                        EscapeString(ExpectedChunk[i]).c_str(),
                        EscapeString(chunks[i]).c_str());
                    VERIFY_FAIL();
                }
            }

            if (ExpectedChunk.size() != chunks.size())
            {
                LogError(
                    "Input: '%hs', Number of chunks do not match. Expected: %zu, Actual: %zu",
                    EscapeString(allData).c_str(),
                    ExpectedChunk.size(),
                    chunks.size());
                VERIFY_FAIL();
            }

            LogInfo("HTTPChunkReaderSplitReads success. Input: %hs", EscapeString(allData).c_str());
        };

        runTest({"3\r\nfo", "o\r\n3\r\nbar"}, {"foo", "bar"});
        runTest({"1\r\n", "a\r\n\r\n"}, {"a"});

        runTest({"c\r\nlf\n", "in\r\nchunk\r\n3\r\nEOF"}, {"lf\nin\r\nchunk", "EOF"});
        runTest({"15\r\n\r\nchunkstartingwithlf\r\n", "3\r\nEOF"}, {"\r\nchunkstartingwithlf", "EOF"});

        runTest({"3", "\r\nfoo\r\n3\r\nbar"}, {"foo", "bar"});
        runTest({"3\r\nfoo\r\n3\r\nbar\r\n0", "\r\n\r\n"}, {"foo", "bar"});
    }

    WSLC_TEST_METHOD(WriteHandleContent)
    {
        // Validate that writing to a pipe works as expected.
        {
            const std::string expectedData = "Pipe-test";
            std::vector<char> writeBuffer{expectedData.begin(), expectedData.end()};

            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::string readData;
            wsl::windows::common::io::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::io::ReadHandle>(std::move(readPipe), [&](const gsl::span<char>& buffer) {
                if (!buffer.empty())
                {
                    readData.append(buffer.data(), buffer.size());
                }
            }));

            io.AddHandle(std::make_unique<WriteHandle>(std::move(writePipe), writeBuffer));

            io.Run({});

            VERIFY_ARE_EQUAL(expectedData, readData);
        }

        // Validate that writing to files work as expected.
        // Use a large buffer to make sure that overlapped writes correctly handle offsets.
        {
            constexpr size_t fileSize = 50 * 1024 * 1024;

            std::vector<char> writeBuffer(fileSize);
            for (size_t i = 0; i < fileSize; i++)
            {
                writeBuffer[i] = static_cast<char>(i % 251);
            }

            auto outputFile = wil::open_or_create_file(L"write-handle-test", GENERIC_WRITE | GENERIC_READ, 0, nullptr);

            auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
                outputFile.reset();
                std::filesystem::remove("write-handle-test");
            });

            wsl::windows::common::io::MultiHandleWait io;
            io.AddHandle(std::make_unique<WriteHandle>(outputFile.get(), writeBuffer));
            io.Run({});

            VERIFY_ARE_NOT_EQUAL(SetFilePointer(outputFile.get(), 0, nullptr, FILE_BEGIN), INVALID_SET_FILE_POINTER);

            LARGE_INTEGER size{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetFileSizeEx(outputFile.get(), &size));
            VERIFY_ARE_EQUAL(static_cast<long long>(fileSize), size.QuadPart);

            std::vector<char> readBuffer(fileSize);
            DWORD bytesRead = 0;
            VERIFY_IS_TRUE(ReadFile(outputFile.get(), readBuffer.data(), static_cast<DWORD>(fileSize), &bytesRead, nullptr));
            VERIFY_ARE_EQUAL(static_cast<DWORD>(fileSize), bytesRead);
            VERIFY_IS_TRUE(readBuffer == writeBuffer);
        }

        // Validate that WriteHandle behaves correctly when its buffer is fully written, and CompleteOnDrained is false.
        {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            PartialHandleRead reader(readPipe.get());

            wsl::windows::common::io::MultiHandleWait io;
            auto writerHandle =
                std::make_unique<WriteHandle>(wsl::windows::common::io::HandleWrapper{std::move(writePipe)}, std::vector<char>{}, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // A reusable writer with nothing queued is Idle, so Run() has no handle to wait on and returns.
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            // First write: a single Push() transitions the writer out of Idle and is delivered.
            std::string first = "first-chunk";
            writer->Push(gsl::make_span(first.data(), first.size()));
            VERIFY_ARE_EQUAL(writer->PendingBytes(), first.size());
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.ExpectConsume(first);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));

            // Reuse: the writer returned to Idle (not Completed) so it is still registered, and several
            // queued Push() calls accumulate and are written in order during the next Run().
            std::string a = "aaa";
            std::string b = "bbbb";
            std::string c = "cc";
            writer->Push(gsl::make_span(a.data(), a.size()));
            writer->Push(gsl::make_span(b.data(), b.size()));
            writer->Push(gsl::make_span(c.data(), c.size()));
            VERIFY_ARE_EQUAL(writer->PendingBytes(), a.size() + b.size() + c.size());
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.ExpectConsume(a + b + c);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));

            // Close the writer.
            writer->SetCompleteOnDrained(true);
            std::string exit = "exit";
            writer->Push(gsl::make_span(exit.data(), exit.size()));

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            reader.Expect(exit);
            reader.ExpectClosed();
        }
    }

    TEST_METHOD(WriteNamedPipeContent)
    {
        using wsl::windows::common::io::HandleWrapper;
        using wsl::windows::common::io::MultiHandleWait;
        using wsl::windows::common::io::WriteNamedPipe;

        auto createServerPipe = [](const std::wstring& name) {
            wil::unique_hfile pipe(CreateNamedPipeW(
                name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr));
            THROW_LAST_ERROR_IF(!pipe);

            return pipe;
        };

        auto connect = [](const std::wstring& name) {
            for (;;)
            {
                wil::unique_hfile client(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
                if (client)
                {
                    return client;
                }

                const auto error = GetLastError();
                THROW_WIN32_IF(error, error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND);

                THROW_IF_WIN32_BOOL_FALSE(WaitNamedPipeW(name.c_str(), 30 * 1000));
            }
        };

        auto push = [](WriteNamedPipe& writer, std::string& data) { writer.Push(gsl::make_span(data.data(), data.size())); };

        // Scenario 1: a payload queued before any client exists is delivered once a client connects,
        // and PendingBytes() drops to zero after the write drains.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // Connect a client up-front; the writer completes the handshake during Run().
            auto client = connect(name);
            PartialHandleRead reader(client.get());

            std::string expected = "hello-named-pipe";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 2: multiple Push() calls accumulate and are delivered, in order, as a single stream.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            auto client = connect(name);
            PartialHandleRead reader(client.get());

            std::string a = "aaaa";
            std::string b = "bbbbbb";
            std::string c = "cc";
            push(*writer, a);
            push(*writer, b);
            push(*writer, c);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), a.size() + b.size() + c.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(a + b + c);
        }

        // Scenario 3: when the connected client disconnects, the next write fails and the writer
        // reconnects, resuming delivery to a new client without losing the buffered payload.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, true, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            // Phase 1: the first client connects, reads the first payload, then disconnects (the reader
            // and client are scoped so the reader thread joins before the client handle closes).
            std::string first = "first-payload";
            {
                auto client1 = connect(name);
                PartialHandleRead reader1(client1.get());

                push(*writer, first);
                VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
                reader1.Expect(first);
            }

            // Phase 2: the next write fails against the now-closed client, triggering a reconnect. A
            // second client connects while Run() performs the reconnect and receives the buffered payload.
            std::string second = "second-payload";
            push(*writer, second);

            wil::unique_hfile client2;
            std::thread connector([&]() { client2 = connect(name); });
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            connector.join();

            PartialHandleRead reader2(client2.get());
            reader2.Expect(second);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 4: a writer over an already-connected handle (Connected=true) skips the connection
        // handshake and behaves like a persistent WriteHandle, writing queued data straight to the handle.
        {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            PartialHandleRead reader(readPipe.get());

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{std::move(writePipe)}, false, true);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            std::string expected = "no-reconnect-path";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));

            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }

        // Scenario 5: Validate that the named pipe is connected if constructed with Connected = false.
        {
            const auto name = wsl::windows::common::helpers::GetUniquePipeName();

            MultiHandleWait io;
            auto writerHandle = std::make_unique<WriteNamedPipe>(HandleWrapper{createServerPipe(name)}, false, false);
            auto* writer = writerHandle.get();
            io.AddHandle(std::move(writerHandle));

            std::string expected = "handshake-without-reconnect";
            push(*writer, expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), expected.size());

            // Connect the client after the payload is queued; the writer completes the handshake during Run().
            wil::unique_hfile client;
            std::thread connector([&]() { client = connect(name); });
            VERIFY_IS_TRUE(io.Run(std::chrono::seconds(30)));
            connector.join();

            PartialHandleRead reader(client.get());
            reader.Expect(expected);
            VERIFY_ARE_EQUAL(writer->PendingBytes(), static_cast<size_t>(0));
        }
    }

    TEST_METHOD(DockerIORelay)
    {
        using namespace wsl::windows::common::io;

        auto runTest = [](const std::vector<char>& Input, const std::string& ExpectedStdout, const std::string& ExpectedStderr) {
            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);
            auto [stderrRead, stderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            MultiHandleWait io;

            std::string readStdout;
            std::string readStderr;

            io.AddHandle(std::make_unique<DockerIORelayHandle>(
                std::move(readPipe), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));
            io.AddHandle(std::make_unique<WriteHandle>(std::move(writePipe), Input));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stdoutRead), [&](const auto& buffer) { readStdout.append(buffer.data(), buffer.size()); }));

            io.AddHandle(std::make_unique<ReadHandle>(
                std::move(stderrRead), [&](const auto& buffer) { readStderr.append(buffer.data(), buffer.size()); }));

            io.Run({});

            VERIFY_ARE_EQUAL(ExpectedStdout, readStdout);
            VERIFY_ARE_EQUAL(ExpectedStderr, readStderr);
        };

        auto insert = [](std::vector<char>& buffer, auto fd, const std::string& content) {
            DockerIORelayHandle::MultiplexedHeader header;
            header.Fd = fd;
            header.Length = htonl(static_cast<uint32_t>(content.size()));

            buffer.insert(buffer.end(), (char*)&header, ((char*)&header) + sizeof(header));
            buffer.insert(buffer.end(), content.begin(), content.end());
        };

        {
            std::vector<char> input;
            insert(input, 1, "foo");
            insert(input, 1, "bar");
            insert(input, 2, "stderr");
            insert(input, 2, "stderrAgain");
            insert(input, 1, "stdOutAgain");

            runTest(input, "foobarstdOutAgain", "stderrstderrAgain");
        }

        {
            std::vector<char> input;
            insert(input, 0, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }

        {
            std::vector<char> input;
            insert(input, 12, "foo");

            VERIFY_ARE_EQUAL(wil::ResultFromException([&]() { runTest(input, "", ""); }), E_INVALIDARG);
        }

        // Validate that behavior is correct if a read spans across multiple streams.
        {
            std::vector<char> input;

            std::string largeStdout(LX_RELAY_BUFFER_SIZE + 150, 'a');
            std::string largeStderr(LX_RELAY_BUFFER_SIZE + 12, 'b');
            insert(input, 1, largeStdout);
            insert(input, 2, largeStderr);
            insert(input, 1, "regularStdout");

            runTest(input, largeStdout + "regularStdout", largeStderr);
        }

        // Validate that behavior is correct with various input sizes.
        {
            const std::string marker1 = "--start--";
            const std::string marker2 = "--end--";

            auto runTest = [&](size_t payloadSize) {
                std::vector<char> input;
                insert(input, 1, marker1);
                insert(input, 1, std::string(payloadSize, 'A'));
                insert(input, 1, marker2);
                const std::string expected = marker1 + std::string(payloadSize, 'A') + marker2;

                auto [inputRead, inputWrite] =
                    wsl::windows::common::wslutil::OpenAnonymousPipe(static_cast<DWORD>(input.size() + 1), true, false);
                auto [stdoutRead, stdoutWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(4096, true, true);
                auto [stderrRead, stderrWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(4096, true, true);

                DWORD written = 0;
                THROW_IF_WIN32_BOOL_FALSE(WriteFile(inputWrite.get(), input.data(), static_cast<DWORD>(input.size()), &written, nullptr));
                VERIFY_ARE_EQUAL(written, static_cast<DWORD>(input.size()));

                std::string output;
                MultiHandleWait io;

                io.AddHandle(std::make_unique<DockerIORelayHandle>(
                    std::move(inputRead), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));

                io.AddHandle(std::make_unique<ReadHandle>(std::move(stdoutRead), [&](const auto& buffer) {
                    output.append(buffer.data(), buffer.size());
                    if (output.find(marker2) != std::string::npos)
                    {
                        io.Cancel();
                    }
                }));

                io.Run(std::chrono::seconds(60));

                VERIFY_ARE_EQUAL(expected, output);
            };

            for (const size_t payloadSize : {1, 100, 4096, 8192, 32768, 64036, 65535, 65536, 65537, 65556, 65571, 65572, 65576, 130000})
            {
                runTest(payloadSize);
            }
        }
    }

    TEST_METHOD(RelayHandleLargeBuffer)
    {
        using namespace wsl::windows::common::io;

        auto [srcRead, srcWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);
        auto [dstRead, dstWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, true);

        // A payload larger than the relay read buffer forces several read -> write cycles through the
        // RelayHandle's reused WriteHandle.
        const std::string payload(LX_RELAY_BUFFER_SIZE * 4 + 123, 'x');

        MultiHandleWait io;

        io.AddHandle(std::make_unique<WriteHandle>(std::move(srcWrite), std::vector<char>(payload.begin(), payload.end())));
        io.AddHandle(std::make_unique<RelayHandle<>>(std::move(srcRead), std::move(dstWrite)));

        // Collect the relayed output.
        std::string output;
        io.AddHandle(std::make_unique<ReadHandle>(
            std::move(dstRead), [&](const gsl::span<char>& buffer) { output.append(buffer.data(), buffer.size()); }));

        io.Run({});

        VERIFY_ARE_EQUAL(payload.size(), output.size());
        VERIFY_IS_TRUE(payload == output);
    }

    TEST_METHOD(HttpHeaderEndDetector)
    {
        // Returns the index of the byte of header end, or -1 if the header never ends.
        const auto headerEndIndex = [](std::string_view input) {
            wsl::windows::common::HttpHeaderEndDetector detector;
            for (size_t i = 0; i < input.size(); i++)
            {
                if (detector.Consume(input[i]))
                {
                    return static_cast<int>(i);
                }
            }

            return -1;
        };

        VERIFY_ARE_EQUAL(3, headerEndIndex("\r\n\r\n"));
        VERIFY_ARE_EQUAL(4, headerEndIndex("a\r\n\r\n"));
        VERIFY_ARE_EQUAL(7, headerEndIndex("a\r\nb\r\n\r\n"));
        VERIFY_ARE_EQUAL(4, headerEndIndex("\r\r\n\r\n"));
        VERIFY_ARE_EQUAL(3, headerEndIndex("\r\n\r\nbody"));

        VERIFY_ARE_EQUAL(-1, headerEndIndex(""));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("Header: value\r\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("HTTP/1.1 200 OK\r\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\r\n\r"));

        // Detection is strict.
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\n\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\r\n\n"));
        VERIFY_ARE_EQUAL(-1, headerEndIndex("\n\r\n"));
    }

    WSLC_TEST_METHOD(ContainerRecoveryFromStorage)
    {
        auto restore = ResetTestSession(); // Required to access the storage folder.

        std::string containerName = "test-container";
        ULONGLONG originalStateChangedAt{};
        ULONGLONG originalCreatedAt{};

        // Phase 1: Create session and container, then stop the container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Create and start a container
            WSLCContainerLauncher launcher("debian:latest", containerName.c_str(), {"sleep", "9999"});

            auto container = launcher.Launch(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Stop the container so it can be recovered and deleted later
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Capture StateChangedAt and CreatedAt before the session is destroyed.
            auto [containers, ports] = ListContainers(session.get());
            VERIFY_ARE_EQUAL(containers.size(), 1);
            originalStateChangedAt = containers[0].StateChangedAt;
            originalCreatedAt = containers[0].CreatedAt;
            VERIFY_IS_TRUE(originalStateChangedAt > 0);
            VERIFY_IS_TRUE(originalCreatedAt > 0);
        }

        // Phase 2: Create new session from same storage, recover and delete container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            auto container = OpenContainer(session.get(), containerName);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that StateChangedAt was correctly restored from the Docker timestamp.
            auto [containers, ports] = ListContainers(session.get());
            VERIFY_ARE_EQUAL(containers.size(), 1);

            // StateChangedAt may differ by ~1s between live (event time) and recovery (FinishedAt).
            auto stateChangedAtDiff = (containers[0].StateChangedAt > originalStateChangedAt)
                                          ? (containers[0].StateChangedAt - originalStateChangedAt)
                                          : (originalStateChangedAt - containers[0].StateChangedAt);
            VERIFY_IS_TRUE(stateChangedAtDiff <= 60);
            VERIFY_ARE_EQUAL(containers[0].CreatedAt, originalCreatedAt);

            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }

        // Phase 3: Create new session from same storage, verify the container is not listed.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), WSLC_E_CONTAINER_NOT_FOUND);
        }
    }

    WSLC_TEST_METHOD(ContainerVolumeAndPortRecoveryFromStorage)
    {
        auto restore = ResetTestSession();

        std::string containerName = "test-recovery-volumes-ports";

        auto hostFolder = std::filesystem::current_path() / "test-recovery-volume";
        std::filesystem::create_directories(hostFolder);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(hostFolder, ec);
        });

        // Create a test file in the host folder
        std::ofstream testFile(hostFolder / "test.txt");
        testFile << "recovery-test-content";
        testFile.close();

        // Create session and container with volumes and ports (but don't start it)
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));

            WSLCContainerLauncher launcher(
                "python:3.12-alpine",
                containerName,
                {"python3", "-m", "http.server", "--directory", "/volume"},
                {"PYTHONUNBUFFERED=1"},
                "bridge");

            launcher.AddPort(1250, 8000, AF_INET);
            launcher.AddVolume(hostFolder.wstring(), "/volume", false);

            // Create container but don't start it
            auto container = launcher.Create(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
        }

        // Recover the container in a new session, start it and verify volume and port mapping works.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));
            auto container = OpenContainer(session.get(), containerName);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr, nullptr));

            auto initProcess = container.GetInitProcess();
            WaitForOutput(initProcess.GetStdHandle(1), "Serving HTTP on 0.0.0.0 port 8000");

            // A 200 response also indicates the test file is available so volume was mounted correctly.
            ExpectHttpResponse(L"http://127.0.0.1:1250/test.txt", 200);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
        }

        // Delete the host folder to simulate volume folder being missing on recovery
        cleanup.reset();

        // Create a new session - this should succeed even though the volume folder is gone
        auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test-vp", true, WSLCNetworkingModeNAT));

        wil::com_ptr<IWSLCContainer> container;
        auto hr = session->OpenContainer(containerName.c_str(), &container);

        VERIFY_ARE_EQUAL(hr, WSLC_E_CONTAINER_NOT_FOUND);
    }

    TEST_METHOD(ContainerRecoveryFromStorageInvalidMetadata)
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "container", "rm", "-f", "test-invalid-metadata"});
        });

        {
            // Create a docker container that has no metadata.
            auto result = RunCommand(
                m_defaultSession.get(),
                {"/usr/bin/docker", "container", "create", "--name", "test-invalid-metadata", "debian:latest"});
            VERIFY_ARE_EQUAL(result.Code, 0L);
        }

        {
            ResetTestSession();
            // Try to open the container - this should fail due to missing metadata.
            wil::com_ptr<IWSLCContainer> container;
            auto hr = m_defaultSession->OpenContainer("test-invalid-metadata", &container);
            VERIFY_ARE_EQUAL(hr, E_UNEXPECTED);
        }
    }

    WSLC_TEST_METHOD(SessionManagement)
    {
        auto manager = OpenSessionManager();

        auto expectSessions = [&](const std::vector<std::wstring>& expectedSessions) {
            wil::unique_cotaskmem_array_ptr<WSLCSessionListEntry> sessions;
            VERIFY_SUCCEEDED(manager->ListSessions(&sessions, sessions.size_address<ULONG>()));

            std::set<std::wstring> displayNames;
            for (const auto& e : sessions)
            {
                auto [_, inserted] = displayNames.insert(e.DisplayName);

                VERIFY_IS_TRUE(inserted);
            }

            for (const auto& e : expectedSessions)
            {
                auto it = displayNames.find(e);
                if (it == displayNames.end())
                {
                    LogError("Session not found: %ls", e.c_str());
                    VERIFY_FAIL();
                }

                displayNames.erase(it);
            }

            for (const auto& e : displayNames)
            {
                LogError("Unexpected session found: %ls", e.c_str());
                VERIFY_FAIL();
            }
        };

        auto create = [this](LPCWSTR Name, WSLCSessionFlags Flags) {
            return CreateSession(GetDefaultSessionSettings(Name), Flags);
        };

        // Validate that non-persistent sessions are dropped when released
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsNone);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that persistent sessions are only dropped when explicitly terminated.
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});
            session1 = create(L"session-1", WSLCSessionFlagsOpenExisting);

            VERIFY_SUCCEEDED(session1->Terminate());
            session1.reset();
            expectSessions({c_testSessionName});
        }

        // Validate that sessions can be reopened by name.
        {
            auto session1 = create(L"session-1", WSLCSessionFlagsPersistent);
            expectSessions({L"session-1", c_testSessionName});

            session1.reset();
            expectSessions({L"session-1", c_testSessionName});

            auto session1Copy =
                create(L"session-1", static_cast<WSLCSessionFlags>(WSLCSessionFlagsPersistent | WSLCSessionFlagsOpenExisting));

            expectSessions({L"session-1", c_testSessionName});

            // Verify that name conflicts are correctly handled.
            auto settings = GetDefaultSessionSettings(L"session-1");

            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(manager->CreateSession(&settings, WSLCSessionFlagsPersistent, nullptr, &session), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            VERIFY_SUCCEEDED(session1Copy->Terminate());
            WSLCSessionState state{};
            VERIFY_SUCCEEDED(session1Copy->GetState(&state));
            VERIFY_ARE_EQUAL(state, WSLCSessionStateTerminated);
            expectSessions({c_testSessionName});

            // Validate that a new session is created if WSLCSessionFlagsOpenExisting is set and no match is found.
            auto session2 = create(L"session-2", static_cast<WSLCSessionFlags>(WSLCSessionFlagsOpenExisting));
        }

        // Validate that elevated session can't be opened by non-elevated tokens
        {
            auto elevatedSession = create(L"elevated-session", WSLCSessionFlagsNone);

            auto nonElevatedToken = GetNonElevatedToken(TokenImpersonation);
            auto revert = wil::impersonate_token(nonElevatedToken.get());
            auto nonElevatedSession = create(L"non-elevated-session", WSLCSessionFlagsNone);

            // Validate that non-elevated tokens can't open an elevated session.
            wil::com_ptr<IWSLCSession> openedSession;
            ULONG elevatedId{};
            VERIFY_SUCCEEDED(elevatedSession->GetId(&elevatedId));
            VERIFY_ARE_EQUAL(manager->OpenSession(elevatedId, &openedSession), HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED));
            VERIFY_IS_FALSE(!!openedSession);

            // Validate that non-elevated tokens can open non-elevated sessions.
            ULONG nonElevatedId{};
            VERIFY_SUCCEEDED(nonElevatedSession->GetId(&nonElevatedId));
            VERIFY_SUCCEEDED(manager->OpenSession(nonElevatedId, &openedSession));
            VERIFY_IS_TRUE(!!openedSession);
        }
    }

    static void ValidateHandleOutput(HANDLE handle, const std::string& expectedOutput)
    {
        VERIFY_ARE_EQUAL(EscapeString(expectedOutput), EscapeString(ReadToString(handle)));
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

            ValidateHandleOutput(initProcess.GetStdHandle(WSLCFDTty).get(), "Type: devpts\r\n");
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
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.get(), "line1\n", 6, nullptr, nullptr));

            reader.Expect("line1\n");
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.get(), "line2\n", 6, nullptr, nullptr));
            reader.Expect("line1\nline2\n");

            containerStdin.reset();
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
            VERIFY_ARE_EQUAL(container.Get().Logs(static_cast<WSLCLogsFlags>(0x4), &stdoutHandle, &stderrHandle, 0, 0, 0), E_INVALIDARG);
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
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(containerStdin.get(), "OK\n", 3, nullptr, nullptr));

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

        // Test valid labels
        {
            WSLCContainerLauncher launcher("debian:latest", "test-labels", {"echo", "OK"});

            for (const auto& [key, value] : labels)
            {
                launcher.AddLabel(key, value);
            }

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(labels, container.Labels());

            // Keep the container alive after the handle is dropped so we can validate labels are persisted across sessions.
            container.SetDeleteOnClose(false);
        }

        {
            // Restarting the test session will force the container to be reloaded from storage.
            ResetTestSession();

            // Validate that labels are correctly loaded.
            auto container = OpenContainer(m_defaultSession.get(), "test-labels");
            VERIFY_ARE_EQUAL(labels, container.Labels());
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

            PartialHandleRead originalReader(originalStdout.get());
            PartialHandleRead attachedReader(stdoutHandle.Get());

            // Write content on the original stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(originalStdin.get(), "line1\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\n");
            attachedReader.Expect("line1\n");

            // Write content on the attached stdin.
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(stdinHandle.Get(), "line2\n", 6, nullptr, nullptr));

            // Content should be relayed on both stdouts.
            originalReader.Expect("line1\nline2\n");
            attachedReader.Expect("line1\nline2\n");

            // Close the original stdin.
            originalStdin.reset();

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

            PartialHandleRead originalReader(originalStdout.get());
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

            PartialHandleRead originalReader(originalTty.get());
            PartialHandleRead attachedReader(attachedTty.Get());

            // Read the prompt from the original tty (hardcoded bytes since behavior is constant).
            auto prompt = originalReader.ReadBytes(13);
            VerifyPatternMatch(prompt, "*root@*");

            // Resize the tty to force the prompt to redraw.
            process.Get().ResizeTty(61, 81);

            auto attachedPrompt = attachedReader.ReadBytes(13);
            VerifyPatternMatch(attachedPrompt, "*root@*");

            // Close the tty.
            originalTty.reset();
            attachedTty.Reset();

            originalReader.ExpectClosed();
            attachedReader.ExpectClosed();
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
            PartialHandleRead reader(tty.get());
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
            VERIFY_ARE_EQUAL(m_defaultSession->PullImage(name, nullptr, nullptr, nullptr), E_INVALIDARG);

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

            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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

        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
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
                validateDetaches(tty.get(), tty.get(), DetachSequence);
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

                validateDetaches(tty.get(), tty.get(), DetachSequence);
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

    TEST_METHOD(ImageParsing)
    {
        using wsl::windows::common::wslutil::ParseImage;

        auto ValidateImageParsing = [](const std::string& input, const std::string& expectedRepo, const std::optional<std::string>& expectedTag) {
            auto [repo, tag] = ParseImage(input);
            VERIFY_ARE_EQUAL(repo, expectedRepo);
            VERIFY_ARE_EQUAL(tag.value_or("<empty>"), expectedTag.value_or("<empty>"));
        };

        ValidateImageParsing("ubuntu:22.04", "ubuntu", "22.04");
        ValidateImageParsing("ubuntu", "ubuntu", {});
        ValidateImageParsing("library/ubuntu:latest", "library/ubuntu", "latest");
        ValidateImageParsing("myregistry.io:5000/myimage:v1", "myregistry.io:5000/myimage", "v1");
        ValidateImageParsing("myregistry.io:5000/myimage", "myregistry.io:5000/myimage", {});

        ValidateImageParsing(
            "registry.example.com:8080/org/project/image:stable", "registry.example.com:8080/org/project/image", "stable");

        ValidateImageParsing("localhost:5000/myimage:latest", "localhost:5000/myimage", "latest");
        ValidateImageParsing("ghcr.io/owner/repo:sha-abc123", "ghcr.io/owner/repo", "sha-abc123");

        ValidateImageParsing(
            "ubuntu@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        // Validate that the digest takes precedence over the tag.
        ValidateImageParsing(
            "ubuntu:latest@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing(
            "myregistry.io:5000/myimage@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "myregistry.io:5000/myimage",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing(
            "ubuntu:22.04@sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30",
            "ubuntu",
            "sha256:2e863c44b718727c860746568e1d54afd13b2fa71b160f5cd9058fc436217b30");

        ValidateImageParsing("pytorch/pytorch", "pytorch/pytorch", {});

        // Invalid inputs
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage(""); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage(":debian:latest"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage("debian:latest@"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage(""); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage(":"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage("a:"); }), E_INVALIDARG);
        VERIFY_ARE_EQUAL(wil::ResultFromException([]() { ParseImage(":b"); }), E_INVALIDARG);
    }

    TEST_METHOD(RepoParsing)
    {
        using wsl::windows::common::wslutil::NormalizeRepo;

        auto ValidateRepoParsing = [](const std::string& input, const std::string& expectedServer, const std::string& expectedPath) {
            auto [server, path] = NormalizeRepo(input);
            VERIFY_ARE_EQUAL(server, expectedServer);
            VERIFY_ARE_EQUAL(path, expectedPath);
        };

        ValidateRepoParsing("ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("docker.io/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("index.docker.io/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("index.docker.io/library/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("docker.io/library/ubuntu", "docker.io", "library/ubuntu");
        ValidateRepoParsing("microsoft.com/ubuntu", "microsoft.com", "ubuntu");
        ValidateRepoParsing("microsoft.com:80/ubuntu", "microsoft.com:80", "ubuntu");
        ValidateRepoParsing("microsoft.com:80/ubuntu/foo/bar", "microsoft.com:80", "ubuntu/foo/bar");
        ValidateRepoParsing("127.0.0.1:80/ubuntu/foo/bar", "127.0.0.1:80", "ubuntu/foo/bar");
        ValidateRepoParsing("pytorch/pytorch", "docker.io", "pytorch/pytorch");
        ValidateRepoParsing("2001:0db8:85a3:0000:0000:8a2e:0370:7334/path", "2001:0db8:85a3:0000:0000:8a2e:0370:7334", "path");
        ValidateRepoParsing(
            "2001:0db8:85a3:0000:0000:8a2e:0370:7334:80/path", "2001:0db8:85a3:0000:0000:8a2e:0370:7334:80", "path");
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

            // Verify the warning matches the expected localized message for the corrupt container.
            auto warnings = warningCallback->GetWarnings();
            auto expectedWarning = std::format(
                L"wsl: {}\n",
                wsl::shared::Localization::MessageWslcFailedToRecoverContainer(wsl::shared::string::MultiByteToWide(containerId)));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == expectedWarning; }));

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

            // Verify the warning matches the expected localized message for the missing volume.
            auto warnings = warningCallback->GetWarnings();
            auto expectedWarning =
                std::format(L"wsl: {}\n", wsl::shared::Localization::MessageWslcFailedToRecoverVolume(L"wslc-test-warning-recovery"));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == expectedWarning; }));

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

            auto warnings = warningCallback->GetWarnings();
            auto expectedWarning = std::format(L"wsl: {}\n", wsl::shared::Localization::MessageWslcFailedToRecoverVolume(c_volumeName));

            VERIFY_IS_TRUE(std::ranges::any_of(warnings, [&](const auto& w) { return w == expectedWarning; }));

            // Clean up the volume from Docker's metadata.
            ExpectCommandResult(session.get(), {"/usr/bin/docker", "volume", "rm", "-f", c_volumeName}, 0);

            VERIFY_SUCCEEDED(session->Terminate());
        }
    }
};
