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
#include "WSLCProcessLauncher.h"
#include "WSLCContainerLauncher.h"
#include "WslCoreFilesystem.h"

using namespace std::literals::chrono_literals;
using namespace wsl::windows::common::registry;
using wsl::windows::common::RunningWSLCContainer;
using wsl::windows::common::RunningWSLCProcess;
using wsl::windows::common::WSLCContainerLauncher;
using wsl::windows::common::WSLCProcessLauncher;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::WriteHandle;
using namespace wsl::windows::common::wslutil;

extern std::wstring g_testDataPath;
extern bool g_fastTestRun;

class WSLCTests
{
    WSLC_TEST_CLASS(WSLCTests)

    wil::unique_mta_usage_cookie m_mtaCookie;
    WSADATA m_wsadata;
    std::filesystem::path m_storagePath;
    WSLCSessionSettings m_defaultSessionSettings{};
    wil::com_ptr<IWSLCSession> m_defaultSession;
    static inline auto c_testSessionName = L"wslc-test";

    void LoadTestImage(std::string_view imageName, IWSLCSession* session = nullptr)
    {
        std::filesystem::path imagePath = GetTestImagePath(imageName);
        wil::unique_hfile imageFile{
            CreateFileW(imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        THROW_LAST_ERROR_IF(!imageFile);

        LARGE_INTEGER fileSize{};
        THROW_LAST_ERROR_IF(!GetFileSizeEx(imageFile.get(), &fileSize));

        THROW_IF_FAILED(
            (session ? session : m_defaultSession.get())->LoadImage(ToCOMInputHandle(imageFile.get()), nullptr, fileSize.QuadPart));
    }

    TEST_CLASS_SETUP(TestClassSetup)
    {
        THROW_IF_FAILED(CoIncrementMTAUsage(&m_mtaCookie));
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsadata));

        // The WSLC SDK tests use this same storage to reduce pull overhead.
        m_storagePath = std::filesystem::current_path() / "test-storage";
        m_defaultSessionSettings = GetDefaultSessionSettings(c_testSessionName, true, WSLCNetworkingModeVirtioProxy);
        m_defaultSession = CreateSession(m_defaultSessionSettings);

        wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
        VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, &images, images.size_address<ULONG>()));

        auto hasImage = [&](const std::string& imageName) {
            return std::ranges::any_of(
                images.get(), images.get() + images.size(), [&](const auto& e) { return e.Image == imageName; });
        };

        if (!hasImage("debian:latest"))
        {
            LoadTestImage("debian:latest");
        }

        if (!hasImage("python:3.12-alpine"))
        {
            LoadTestImage("python:3.12-alpine");
        }

        PruneResult result;
        VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(nullptr, 0, 0, &result.result));
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

        VERIFY_SUCCEEDED(sessionManager->CreateSession(&sessionSettings, Flags, &session));
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

    TEST_METHOD(GetVersion)
    {
        WSL2_TEST_ONLY();

        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));

        WSLCVersion version{};

        VERIFY_SUCCEEDED(sessionManager->GetVersion(&version));

        VERIFY_ARE_EQUAL(version.Major, WSL_PACKAGE_VERSION_MAJOR);
        VERIFY_ARE_EQUAL(version.Minor, WSL_PACKAGE_VERSION_MINOR);
        VERIFY_ARE_EQUAL(version.Revision, WSL_PACKAGE_VERSION_REVISION);
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

    TEST_METHOD(ListSessionsReturnsSessionWithDisplayName)
    {
        WSL2_TEST_ONLY();

        auto sessionManager = OpenSessionManager();

        // Act: list sessions
        {
            wil::unique_cotaskmem_array_ptr<WSLCSessionInformation> sessions;
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

            wil::unique_cotaskmem_array_ptr<WSLCSessionInformation> sessions;
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

    TEST_METHOD(OpenSessionByNameFindsExistingSession)
    {
        WSL2_TEST_ONLY();

        auto sessionManager = OpenSessionManager();

        // Act: open by the same display name
        wil::com_ptr<IWSLCSession> opened;
        VERIFY_SUCCEEDED(sessionManager->OpenSessionByName(c_testSessionName, &opened));
        VERIFY_IS_NOT_NULL(opened.get());

        // And verify we get ERROR_NOT_FOUND for a nonexistent name
        wil::com_ptr<IWSLCSession> notFound;
        auto hr = sessionManager->OpenSessionByName(L"this-name-does-not-exist", &notFound);
        VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    TEST_METHOD(CreateSessionValidation)
    {
        WSL2_TEST_ONLY();

        auto sessionManager = OpenSessionManager();

        // Reject NULL DisplayName.
        {
            auto settings = GetDefaultSessionSettings(nullptr);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), E_INVALIDARG);
        }

        // Reject DisplayName at exact boundary (no room for null terminator).
        {
            std::wstring boundaryName(std::size(WSLCSessionInformation{}.DisplayName), L'x');
            auto settings = GetDefaultSessionSettings(boundaryName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), E_INVALIDARG);
        }

        // Reject too long DisplayName.
        {
            std::wstring longName(std::size(WSLCSessionInformation{}.DisplayName) + 1, L'x');
            auto settings = GetDefaultSessionSettings(longName.c_str());
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), E_INVALIDARG);
        }

        // Validate that creating a session on a non-existing storage fails if WSLCSessionStorageFlagsNoCreate is set.
        {
            auto settings = GetDefaultSessionSettings(L"storage-not-found");
            settings.StoragePath = L"C:\\does-not-exist";
            settings.StorageFlags = WSLCSessionStorageFlagsNoCreate;
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        }

        // Reject invalid storage flags.
        {
            auto settings = GetDefaultSessionSettings(L"invalid-storage-flags");
            settings.StorageFlags = static_cast<WSLCSessionStorageFlags>(0x2);
            wil::com_ptr<IWSLCSession> session;
            VERIFY_ARE_EQUAL(sessionManager->CreateSession(&settings, WSLCSessionFlagsNone, &session), E_INVALIDARG);
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

    TEST_METHOD(PullImage)
    {
        WSL2_TEST_ONLY();

        {
            HRESULT pullResult = m_defaultSession->PullImage("hello-world:linux", nullptr, nullptr);

            // Skip test if error is due to rate limit.
            if (pullResult == E_FAIL)
            {
                auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
                if (comError.has_value())
                {
                    if (wcsstr(comError->Message.get(), L"toomanyrequests") != nullptr)
                    {
                        LogWarning("Skipping PullImage test due to rate limiting.");
                        return;
                    }
                }
            }

            VERIFY_SUCCEEDED(pullResult);

            // Verify that the image is in the list of images.
            ExpectImagePresent(*m_defaultSession, "hello-world:linux");
            WSLCContainerLauncher launcher("hello-world:linux", "wslc-pull-image-container");

            auto container = launcher.Launch(*m_defaultSession);
            auto result = container.GetInitProcess().WaitAndCaptureOutput();

            VERIFY_ARE_EQUAL(0, result.Code);
            VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);
        }

        {
            std::wstring expectedError =
                L"pull access denied for does-not, repository does not exist or may require 'docker login': denied: requested "
                L"access to the resource is denied";

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("does-not:exist", nullptr, nullptr), WSLC_E_IMAGE_NOT_FOUND);
            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            VERIFY_ARE_EQUAL(expectedError, comError->Message.get());
        }

        // Validate that PullImage() returns the appropriate error if the session is terminated.
        {
            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto cleanup = wil::scope_exit([&]() {
                ResetTestSession(); // Reopen the test session since the session was terminated.
            });

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("hello-world:linux", nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    TEST_METHOD(PullImageAdvanced)
    {
        WSL2_TEST_ONLY();

        // TODO: Enable once custom registries are supported, to avoid hitting public registry rate limits.
        SKIP_TEST_UNSTABLE();

        auto validatePull = [&](const std::string& Image, const std::optional<std::string>& ExpectedTag = {}) {
            VERIFY_SUCCEEDED(m_defaultSession->PullImage(Image.c_str(), nullptr, nullptr));

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
            settings.NetworkingMode = WSLCNetworkingModeVirtioProxy;
            settings.MemoryMb = 1024;
            auto session = CreateSession(settings);

            VERIFY_ARE_EQUAL(session->PullImage("pytorch/pytorch", nullptr, nullptr), E_FAIL);

            auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();
            VERIFY_IS_TRUE(comError.has_value());

            // The error message can't be compared directly because it contains an unpredicable path:
            // "write /var/lib/docker/tmp/GetImageBlob1760660623: no space left on device"
            if (StrStrW(comError->Message.get(), L"no space left on device") == nullptr)
            {
                LogError("Unexpected error message: %ls", comError->Message.get());
                VERIFY_FAIL();
            }
        }
    }

    TEST_METHOD(ListImages)
    {
        WSL2_TEST_ONLY();

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
            WSLCListImageOptions options{};
            options.Flags = WSLCListImagesFlagsNone;
            options.Reference = "debian";

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
            WSLCListImageOptions options{};
            options.Flags = WSLCListImagesFlagsNone;
            options.Reference = "debian:test-tag1";

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
            WSLCListImageOptions options{};
            options.Flags = WSLCListImagesFlagsDigests;
            options.Reference = "debian:latest";

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

        LogInfo("Test: Before/Since filters");
        {
            // Get all images to find their IDs
            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> allImages;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(nullptr, allImages.addressof(), allImages.size_address<ULONG>()));

            std::string debianId, pythonId;
            for (const auto& image : allImages)
            {
                std::string imageName = image.Image;
                if (imageName == "debian:latest")
                {
                    debianId = image.Hash;
                }
                else if (imageName == "python:3.12-alpine")
                {
                    pythonId = image.Hash;
                }
            }

            VERIFY_IS_FALSE(debianId.empty());
            VERIFY_IS_FALSE(pythonId.empty());

            // Test 'since' filter - images created after debian
            {
                WSLCListImageOptions options{};
                options.Flags = WSLCListImagesFlagsNone;
                options.Since = debianId.c_str();

                wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
                VERIFY_IS_TRUE(images.size() > 0);

                bool foundPython = false;
                for (const auto& image : images)
                {
                    LogInfo("Image: %hs, Hash: %hs, Created: %lld", image.Image, image.Hash, image.Created);
                    if (std::string{image.Image} == "python:3.12-alpine")
                    {
                        foundPython = true;
                    }
                }

                VERIFY_IS_TRUE(foundPython);
            }

            // Test 'before' filter - images created before python
            {
                WSLCListImageOptions options{};
                options.Flags = WSLCListImagesFlagsNone;
                options.Before = pythonId.c_str();
                wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
                VERIFY_IS_TRUE(images.size() > 0);

                bool foundDebian = false;
                for (const auto& image : images)
                {
                    if (std::string{image.Image} == "debian:latest")
                    {
                        foundDebian = true;
                    }
                }

                VERIFY_IS_TRUE(foundDebian);
            }
        }

        LogInfo("Test: Dangling filter");
        {
            // Setup a dangling image
            LoadTestImage("alpine:latest");
            WSLCTagImageOptions tagOptions{};
            tagOptions.Image = "debian:latest";
            tagOptions.Repo = "alpine";
            tagOptions.Tag = "latest";
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));

            auto alpineCleanup = wil::scope_exit([&]() {
                RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "image", "prune", "-f"});
                LOG_IF_FAILED(DeleteImageNoThrow("alpine:latest", WSLCDeleteImageFlagsNone).first);
            });

            // List only dangling images
            WSLCListImageOptions options{};
            options.Flags = WSLCListImagesFlagsDanglingTrue;

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
            options.Flags = WSLCListImagesFlagsDanglingFalse;
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
            // Test with nullptr (no label filter)
            WSLCListImageOptions options{};
            options.Flags = WSLCListImagesFlagsNone;
            options.Labels = nullptr;
            options.LabelsCount = 0;

            wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
            VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));

            // Test with single label filter
            {
                WSLCLabel labels[] = {{.Key = "test.label", .Value = nullptr}};
                options.Labels = labels;
                options.LabelsCount = 1;

                VERIFY_SUCCEEDED(m_defaultSession->ListImages(&options, images.addressof(), images.size_address<ULONG>()));
            }

            // Test with multiple label filters (labels are AND'ed together)
            {
                WSLCLabel labels[] = {{.Key = "test.label1", .Value = nullptr}, {.Key = "test.label2", .Value = "value"}};
                options.Labels = labels;
                options.LabelsCount = 2;

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

    TEST_METHOD(LoadImage)
    {
        WSL2_TEST_ONLY();

        std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "hello-world:latest");
        WSLCContainerLauncher launcher("hello-world:latest", "wslc-load-image-container");

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

        // Validate that invalid tars fail with proper error message and code.
        {
            auto currentExecutableHandle = wil::open_file(wil::GetModuleFileNameW<std::wstring>().c_str());
            VERIFY_IS_TRUE(GetFileSizeEx(currentExecutableHandle.get(), &fileSize));

            VERIFY_ARE_EQUAL(m_defaultSession->LoadImage(ToCOMInputHandle(currentExecutableHandle.get()), nullptr, fileSize.QuadPart), E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }
    }

    TEST_METHOD(ImportImage)
    {
        WSL2_TEST_ONLY();

        auto cleanup =
            wil::scope_exit([&]() { LOG_IF_FAILED(DeleteImageNoThrow("my-hello-world:test", WSLCDeleteImageFlagsNone).first); });

        std::filesystem::path imageTar = std::filesystem::path{g_testDataPath} / L"HelloWorldExported.tar";
        wil::unique_handle imageTarFileHandle{
            CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());

        LARGE_INTEGER fileSize{};
        VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));

        VERIFY_SUCCEEDED(m_defaultSession->ImportImage(
            ToCOMInputHandle(imageTarFileHandle.get()), "my-hello-world:test", nullptr, fileSize.QuadPart));

        ExpectImagePresent(*m_defaultSession, "my-hello-world:test");

        // Validate that containers can be started from the imported image.
        WSLCContainerLauncher launcher("my-hello-world:test", "wslc-import-image-container", {"/hello"});

        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("Hello from Docker!") != std::string::npos);

        // Validate that ImportImage fails if no tag is passed
        {
            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(ToCOMInputHandle(imageTarFileHandle.get()), "my-hello-world", nullptr, fileSize.QuadPart),
                E_INVALIDARG);
        }

        // Validate that invalid tars fail with proper error message and code.
        {
            auto currentExecutableHandle = wil::open_file(wil::GetModuleFileNameW<std::wstring>().c_str());

            VERIFY_IS_TRUE(GetFileSizeEx(currentExecutableHandle.get(), &fileSize));

            VERIFY_ARE_EQUAL(
                m_defaultSession->ImportImage(
                    ToCOMInputHandle(currentExecutableHandle.get()), "invalid-image:test", nullptr, fileSize.QuadPart),
                E_FAIL);

            ValidateCOMErrorMessage(L"archive/tar: invalid tar header");
        }
    }

    TEST_METHOD(DeleteImage)
    {
        WSL2_TEST_ONLY();

        // Prepare alpine image to delete.
        LoadTestImage("alpine:latest");

        // Verify that the image is in the list of images.
        ExpectImagePresent(*m_defaultSession, "alpine:latest");

        // Launch a container to ensure that image deletion fails when in use.
        WSLCContainerLauncher launcher(
            "alpine:latest", "test-delete-container-in-use", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeHost);

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

    void ValidateCOMErrorMessage(const std::optional<std::wstring>& Expected)
    {
        auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();

        if (comError.has_value())
        {
            if (!Expected.has_value())
            {
                LogError("Unexpected COM error: '%ls'", comError->Message.get());
                VERIFY_FAIL();
            }

            VERIFY_ARE_EQUAL(Expected.value(), comError->Message.get());
        }
        else
        {
            if (Expected.has_value())
            {
                LogError("Expected COM error: '%ls' but none was set", Expected->c_str());
                VERIFY_FAIL();
            }
        }
    }

    HRESULT BuildImageFromContext(const std::filesystem::path& contextDir, const WSLCBuildImageOptions* options)
    {
        auto dockerfileHandle = wil::open_file((contextDir / "Dockerfile").c_str());

        auto contextPathStr = contextDir.wstring();
        WSLCBuildImageOptions optionsCopy = *options;
        optionsCopy.ContextPath = contextPathStr.c_str();
        optionsCopy.DockerfileHandle = ToCOMInputHandle(dockerfileHandle.get());

        auto buildResult = m_defaultSession->BuildImage(&optionsCopy, nullptr, nullptr);

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

    TEST_METHOD(BuildImage)
    {
        WSL2_TEST_ONLY();

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
    TEST_METHOD(BuildImageEntrypoint)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageWithContext)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageManyFiles)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageLargeFile)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageMultiStage)
    {
        WSL2_TEST_ONLY();

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
            dockerfile << "RUN echo -n 'WSL containers' > /part.txt\n";
            dockerfile << "\n";
            dockerfile << "FROM debian:latest AS description\n";
            dockerfile << "RUN echo -n 'support multi-stage builds' > /part.txt\n";
            dockerfile << "\n";
            dockerfile << "FROM debian:latest\n";
            dockerfile << "COPY --from=greeting /part.txt /greeting.txt\n";
            dockerfile << "COPY --from=description /part.txt /description.txt\n";
            dockerfile << "CMD [\"sh\", \"-c\", "
                       << "\"echo \\\"$(cat /greeting.txt) $(cat /description.txt)\\\"\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, "wslc-test-build-multistage:latest"));
        ExpectImagePresent(*m_defaultSession, "wslc-test-build-multistage:latest");

        WSLCContainerLauncher launcher("wslc-test-build-multistage:latest", "wslc-build-multistage-container");
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess().WaitAndCaptureOutput();

        VERIFY_ARE_EQUAL(0, result.Code);
        VERIFY_IS_TRUE(result.Output[1].find("WSL containers support multi-stage builds") != std::string::npos);
    }

    TEST_METHOD(BuildImageDockerIgnore)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageFailure)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageFailureShowsBuildOutput)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageStdinDockerfile)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageBuildArgs)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageMultipleTags)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(BuildImageNullHandle)
    {
        WSL2_TEST_ONLY();

        WSLCBuildImageOptions options{.ContextPath = L"C:\\", .DockerfileHandle = {}, .Tags = {nullptr, 0}};

        VERIFY_ARE_EQUAL(m_defaultSession->BuildImage(&options, nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
    }

    TEST_METHOD(BuildImageCancel)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(AnonymousVolumes)
    {
        // TODO: Add more test coverage once anonymous volumes are fully supported and switch to using -v instead of building an image.

        WSL2_TEST_ONLY();

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

        WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-anonymous-volume", {"test", "-d", "/volume"});
        auto container = launcher.Launch(*m_defaultSession);
        auto result = container.GetInitProcess();

        auto containerId = container.Id();

        ValidateProcessOutput(result, {});

        ResetTestSession();

        container.SetDeleteOnClose(false);

        // Manually cleanup the container since the session has been reset.
        auto containerCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            wil::com_ptr<IWSLCContainer> container;
            VERIFY_SUCCEEDED(m_defaultSession->OpenContainer(containerId.c_str(), &container));

            VERIFY_SUCCEEDED(container->Delete(WSLCDeleteFlagsForce));
        });

        // Validate that the session is correctly restarted.
        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
        wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;

        VERIFY_SUCCEEDED(m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

        VERIFY_ARE_EQUAL(containers.size(), 1);
        VERIFY_ARE_EQUAL(containers[0].Id, containerId);
    }

    TEST_METHOD(TagImage)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(InspectImage)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(SaveImage)
    {
        WSL2_TEST_ONLY();
        {
            std::filesystem::path imageTar = GetTestImagePath("hello-world:latest");
            wil::unique_handle imageTarFileHandle{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == imageTarFileHandle.get());
            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(imageTarFileHandle.get(), &fileSize));
            // Load the image from a saved tar
            VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
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
                VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
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

    TEST_METHOD(SynchronousIoCancellation)
    {
        WSL2_TEST_ONLY();

        // Create a blocked operation that will cause the service to get stuck on a ReadFile() call.
        // Because the pipe handle that we're passing in doesn't support overlapped IO, the service will get stuck in a
        // synchronous ReadFile() call. Validate that terminating the session correctly cancels the IO.

        wil::unique_handle pipeRead;
        wil::unique_handle pipeWrite;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(&pipeRead, &pipeWrite, nullptr, 2));

        std::promise<HRESULT> result;

        wil::unique_event testCompleted{wil::EventOptions::ManualReset};
        std::thread operationThread([&]() {
            result.set_value(m_defaultSession->ImportImage(ToCOMInputHandle(pipeRead.get()), "dummy:latest", nullptr, 1024 * 1024));

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

    TEST_METHOD(ExportContainer)
    {
        WSL2_TEST_ONLY();

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
                VERIFY_SUCCEEDED(m_defaultSession->LoadImage(ToCOMInputHandle(imageTarFileHandle.get()), nullptr, fileSize.QuadPart));
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

                VERIFY_SUCCEEDED(m_defaultSession->ImportImage(
                    ToCOMInputHandle(containerTarFileHandle.get()), "test-imported-container:latest", nullptr, fileSize.QuadPart));

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

    TEST_METHOD(CustomDmesgOutput)
    {
        WSL2_TEST_ONLY();
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

            auto session = CreateSession(settings);
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

    TEST_METHOD(TerminationCallback)
    {
        WSL2_TEST_ONLY();

        class DECLSPEC_UUID("7BC4E198-6531-4FA6-ADE2-5EF3D2A04DFF") CallbackInstance
            : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback, IFastRundown>
        {

        public:
            CallbackInstance(std::function<void(WSLCVirtualMachineTerminationReason, LPCWSTR)>&& callback) :
                m_callback(std::move(callback))
            {
            }

            HRESULT OnTermination(WSLCVirtualMachineTerminationReason Reason, LPCWSTR Details) override
            {
                m_callback(Reason, Details);
                return S_OK;
            }

        private:
            std::function<void(WSLCVirtualMachineTerminationReason, LPCWSTR)> m_callback;
        };

        std::promise<std::pair<WSLCVirtualMachineTerminationReason, std::wstring>> promise;

        CallbackInstance callback{[&](WSLCVirtualMachineTerminationReason reason, LPCWSTR details) {
            promise.set_value(std::make_pair(reason, details));
        }};

        WSLCSessionSettings sessionSettings = GetDefaultSessionSettings(L"termination-callback-test");
        sessionSettings.TerminationCallback = &callback;

        auto session = CreateSession(sessionSettings);

        session.reset();
        auto future = promise.get_future();
        auto result = future.wait_for(std::chrono::seconds(30));
        VERIFY_ARE_EQUAL(result, std::future_status::ready);
        auto [reason, details] = future.get();
        VERIFY_ARE_EQUAL(reason, WSLCVirtualMachineTerminationReasonShutdown);
        VERIFY_ARE_NOT_EQUAL(details, L"");
    }

    TEST_METHOD(InteractiveShell)
    {
        WSL2_TEST_ONLY();

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
        WSL2_TEST_ONLY();

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

            VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
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

    TEST_METHOD(VirtioProxyNetworking)
    {
        ValidateNetworking(WSLCNetworkingModeVirtioProxy);
    }

    TEST_METHOD(VirtioProxyNetworkingWithDnsTunneling)
    {
        WINDOWS_11_TEST_ONLY();
        ValidateNetworking(WSLCNetworkingModeVirtioProxy, true);
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
        WSL2_TEST_ONLY();

        auto settings = GetDefaultSessionSettings(L"port-mapping-test");
        settings.NetworkingMode = networkingMode;

        // Reuse the default session if the networking mode matches.
        auto createNewSession = networkingMode != m_defaultSessionSettings.NetworkingMode;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Install socat in the container.
        //
        // TODO: revisit this in the future to avoid pulling packages from the network.
        auto installSocat = WSLCProcessLauncher("/bin/sh", {"/bin/sh", "-c", "tdnf install socat -y"}).Launch(*session);
        ValidateProcessOutput(installSocat, {}, 0, 300 * 1000);

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
        VERIFY_ARE_EQUAL(session->MapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

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
        VERIFY_ARE_EQUAL(session->UnmapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

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

        VERIFY_ARE_EQUAL(
            session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)),
            HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES));

        for (int i = 0; i < c_maxPorts; i++)
        {
            VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i)));
        }
    }

    TEST_METHOD(PortMappingNat)
    {
        ValidatePortMapping(WSLCNetworkingModeNAT);
    }

    TEST_METHOD(PortMappingVirtioProxy)
    {
        ValidatePortMapping(WSLCNetworkingModeVirtioProxy);
    }

    TEST_METHOD(StuckVmTermination)
    {
        WSL2_TEST_ONLY();

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
                    "/win-path*9p*{},relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*", readOnly ? "ro" : "rw");
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
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/not-mounted"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/proc"), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path"));
        }
    }

    TEST_METHOD(WindowsMounts)
    {
        WSL2_TEST_ONLY();
        ValidateWindowsMounts(false);
    }

    TEST_METHOD(WindowsMountsVirtioFs)
    {
        WSL2_TEST_ONLY();
        ValidateWindowsMounts(true);
    }

    // This test case validates that no file descriptors are leaked to user processes.
    TEST_METHOD(Fd)
    {
        WSL2_TEST_ONLY();

        auto result = ExpectCommandResult(
            m_defaultSession.get(), {"/bin/sh", "-c", "echo /proc/self/fd/* && (readlink -v /proc/self/fd/* || true)"}, 0);

        // Note: fd/0 is opened by readlink to read the actual content of /proc/self/fd.
        if (!PathMatchSpecA(result.Output[1].c_str(), "/proc/self/fd/0 /proc/self/fd/1 /proc/self/fd/2\nsocket:*\nsocket:*"))
        {
            LogInfo("Found additional fds: %hs", result.Output[1].c_str());
            VERIFY_FAIL();
        }
    }

    TEST_METHOD(GPU)
    {
        WSL2_TEST_ONLY();

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
                "/usr/lib/wsl/drivers*9p*relatime,aname=*,cache=5,access=client,msize=65536,trans=fd,rfd=*,wfd=*");

            ExpectMount(
                session.get(),
                "/usr/lib/wsl/lib",
                "/usr/lib/wsl/lib none*overlay ro,relatime,lowerdir=/usr/lib/wsl/lib/packaged*");

            // Validate that the mount points are not writeable.
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/drivers/test"}).Code, 1L);
            VERIFY_ARE_EQUAL(RunCommand(session.get(), {"/usr/bin/touch", "/usr/lib/wsl/lib/test"}).Code, 1L);
        }
    }

    TEST_METHOD(Modules)
    {
        WSL2_TEST_ONLY();

        // Sanity check.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 1);

        // Validate that modules can be loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/usr/sbin/modprobe", "xsk_diag"}, 0);

        // Validate that xsk_diag is now loaded.
        ExpectCommandResult(m_defaultSession.get(), {"/bin/sh", "-c", "lsmod | grep ^xsk_diag"}, 0);
    }

    TEST_METHOD(CreateRootNamespaceProcess)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(CrashDumpCollection)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(VhdFormatting)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(NamedVolumesTest)
    {
        WSL2_TEST_ONLY();

        const std::string volumeName = "wslc-test-named-volume";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        // Best-effort cleanup in case of leftovers from a previous failed run.
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
            std::error_code ec;
            std::filesystem::remove(volumeVhdPath, ec);
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Type = "vhd";
        volumeOptions.Options = R"({"SizeBytes":"1073741824"})";

        // Create volume and validate duplicate volume name handling.
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions));
        VERIFY_ARE_EQUAL(m_defaultSession->CreateVolume(&volumeOptions), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

        // Verify volume VHD exists and mount point is present in the VM.
        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));
        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::optional<std::string>{"*ext4*"});

        // Verify the same named volume can be mounted more than once with different container paths.
        {
            WSLCContainerLauncher duplicateNamedVolumes(
                "debian:latest", "named-volume-dup", {"/bin/sh", "-c", "echo duplicated >/data-a/dup.txt ; cat /data-b/dup.txt"});
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-a", false);
            duplicateNamedVolumes.AddNamedVolume(volumeName, "/data-b", true);

            auto duplicateNamedVolumesContainer = duplicateNamedVolumes.Launch(*m_defaultSession);
            auto duplicateNamedVolumesProcess = duplicateNamedVolumesContainer.GetInitProcess();
            ValidateProcessOutput(duplicateNamedVolumesProcess, {{1, "duplicated\n"}});
        }

        // Verify CreateContainer with named volume mounts the volume into the container.
        {
            WSLCContainerLauncher writer(
                "debian:latest", "named-volume-writer", {"/bin/sh", "-c", "echo wslc-named-volume >/data/marker.txt"});
            writer.AddNamedVolume(volumeName, "/data", false);

            auto writerContainer = writer.Launch(*m_defaultSession);
            auto writerProcess = writerContainer.GetInitProcess();
            ValidateProcessOutput(writerProcess, {});

            WSLCContainerLauncher reader("debian:latest", "named-volume-reader", {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "wslc-named-volume\n"}});
        }

        // Verify we cannot delete a named volume while a container references it.
        WSLCContainerLauncher holder("debian:latest", "named-volume-holder", {"sleep", "99999"});
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

        ExpectMount(m_defaultSession.get(), std::format("/mnt/wslc-volumes/{}", volumeName), std::nullopt);
        VERIFY_IS_FALSE(std::filesystem::exists(volumeVhdPath));

        cleanup.release();
    }

    TEST_METHOD(NamedVolumesSessionRecovery)
    {
        WSL2_TEST_ONLY();

        const std::string volumeName = "wslc-test-named-volume";
        const std::string containerName = "wslc-test-container";
        const std::filesystem::path volumeVhdPath = m_storagePath / "volumes" / (volumeName + ".vhdx");

        // Best-effort cleanup in case prior failed runs left artifacts behind.
        RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
        LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));
        {
            std::error_code ec;
            std::filesystem::remove(volumeVhdPath, ec);
        }

        auto cleanup = wil::scope_exit([&]() {
            RunCommand(m_defaultSession.get(), {"/usr/bin/docker", "rm", "-f", containerName});
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

            std::error_code ec;
            std::filesystem::remove(volumeVhdPath, ec);
        });

        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName.c_str();
        volumeOptions.Type = "vhd";
        volumeOptions.Options = R"({"SizeBytes":"1073741824"})";

        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions));
        VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));

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
            WSLCContainerLauncher reader("debian:latest", "wslc-test-container-reader", {"/bin/sh", "-c", "cat /data/marker.txt"});
            reader.AddNamedVolume(volumeName, "/data", true);

            auto readerContainer = reader.Launch(*m_defaultSession);
            auto readerProcess = readerContainer.GetInitProcess();
            ValidateProcessOutput(readerProcess, {{1, "named-volume-recovery\n"}});
        }

        // Stop the session, delete the backing VHD, and restart.
        {
            auto restartSession = ResetTestSession();

            VERIFY_IS_TRUE(std::filesystem::exists(volumeVhdPath));

            std::error_code error;
            VERIFY_IS_TRUE(std::filesystem::remove(volumeVhdPath, error));
            VERIFY_ARE_EQUAL(error, std::error_code{});
        }

        wil::com_ptr<IWSLCContainer> notFound;
        VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(containerName.c_str(), &notFound), E_UNEXPECTED);

        // Deleting the named volume should fail since the volume was not recovered.
        VERIFY_ARE_EQUAL(m_defaultSession->DeleteVolume(volumeName.c_str()), WSLC_E_VOLUME_NOT_FOUND);
    }

    TEST_METHOD(NamedVolumeOptionsParseTest)
    {
        WSL2_TEST_ONLY();

        const std::string volumeName = "wslc-volume-name";

        auto validateInvalidOptionsFailure =
            [&](const std::string& options, HRESULT expectedResult, const std::optional<std::wstring>& expectedMessage = std::nullopt) {
                LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str()));

                auto cleanup = wil::scope_exit([&]() { LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName.c_str())); });

                WSLCVolumeOptions volumeOptions{};
                volumeOptions.Name = volumeName.c_str();
                volumeOptions.Type = "vhd";
                volumeOptions.Options = options.c_str();

                const auto result = m_defaultSession->CreateVolume(&volumeOptions);

                if (result != expectedResult)
                {
                    LogInfo(
                        "CreateVolume mismatch options='%hs' result=0x%08x expected=0x%08x",
                        options.c_str(),
                        static_cast<unsigned int>(result),
                        static_cast<unsigned int>(expectedResult));
                }

                VERIFY_ARE_EQUAL(result, expectedResult);
                if (expectedMessage.has_value())
                {
                    ValidateCOMErrorMessage(expectedMessage);
                }
            };

        validateInvalidOptionsFailure("not-json", WSL_E_INVALID_JSON);
        validateInvalidOptionsFailure(R"({"SizeBytes":"abc"})", WSL_E_INVALID_JSON);
        validateInvalidOptionsFailure(R"({"SizeBytes":"+-1"})", WSL_E_INVALID_JSON);
        validateInvalidOptionsFailure(R"({"SizeBytes":"123abc"})", WSL_E_INVALID_JSON);

        validateInvalidOptionsFailure(R"({"SizeBytes":"18446744073709551616"})", E_INVALIDARG);
        validateInvalidOptionsFailure(R"({"SizeBytes":"-1"})", E_INVALIDARG);
        validateInvalidOptionsFailure(R"({"SizeBytes":"0"})", E_INVALIDARG, L"Invalid size: 0");
        validateInvalidOptionsFailure("{}", E_INVALIDARG, L"Invalid volume options: '{}'");
        validateInvalidOptionsFailure("", WSL_E_INVALID_JSON);
    }

    TEST_METHOD(ListAndInspectNamedVolumesTest)
    {
        WSL2_TEST_ONLY();

        const std::string volumeName1 = "wsla-test-vol1";
        const std::string volumeName2 = "wsla-test-vol2";

        auto cleanup = wil::scope_exit([&]() {
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName1.c_str()));
            LOG_IF_FAILED(m_defaultSession->DeleteVolume(volumeName2.c_str()));
        });

        // Verify empty list is returned when no volumes exist.
        wil::unique_cotaskmem_array_ptr<WSLCVolumeInformation> volumes;
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(0u, volumes.size());

        // Create first volume and verify list returns one entry.
        WSLCVolumeOptions volumeOptions{};
        volumeOptions.Name = volumeName1.c_str();
        volumeOptions.Type = "vhd";
        volumeOptions.Options = R"({"SizeBytes":"1073741824"})";
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions));

        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(std::string(volumes[0].Name), volumeName1);
        VERIFY_ARE_EQUAL(std::string(volumes[0].Type), std::string("vhd"));

        // Create second volume and verify list returns two entries.
        volumeOptions.Name = volumeName2.c_str();
        VERIFY_SUCCEEDED(m_defaultSession->CreateVolume(&volumeOptions));

        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(2u, volumes.size());

        std::set<std::string> names;
        for (const auto& v : volumes)
        {
            names.insert(v.Name);
            VERIFY_ARE_EQUAL(std::string(v.Type), std::string("vhd"));
        }

        VERIFY_IS_TRUE(names.contains(volumeName1));
        VERIFY_IS_TRUE(names.contains(volumeName2));

        // Verify InspectVolume returns correct details.
        wil::unique_cotaskmem_ansistring output;
        VERIFY_SUCCEEDED(m_defaultSession->InspectVolume(volumeName1.c_str(), &output));
        VERIFY_IS_NOT_NULL(output.get());

        auto inspect = wsl::shared::FromJson<wsl::windows::common::wslc_schema::InspectVolume>(output.get());
        VERIFY_ARE_EQUAL(inspect.Name, volumeName1);
        VERIFY_ARE_EQUAL(inspect.Type, std::string("vhd"));
        VERIFY_IS_TRUE(inspect.VhdVolume.has_value());
        VERIFY_ARE_EQUAL(inspect.VhdVolume->SizeBytes, 1073741824ull);
        VERIFY_IS_FALSE(inspect.VhdVolume->HostPath.empty());

        // Verify InspectVolume fails for a non-existent volume.
        output.reset();
        VERIFY_ARE_EQUAL(m_defaultSession->InspectVolume("does-not-exist", &output), WSLC_E_VOLUME_NOT_FOUND);

        // Delete first volume and verify list returns one entry.
        VERIFY_SUCCEEDED(m_defaultSession->DeleteVolume(volumeName1.c_str()));
        VERIFY_SUCCEEDED(m_defaultSession->ListVolumes(volumes.addressof(), volumes.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(1u, volumes.size());
        VERIFY_ARE_EQUAL(std::string(volumes[0].Name), volumeName2);
    }

    TEST_METHOD(CreateContainer)
    {
        WSL2_TEST_ONLY();

        // Test a simple container start.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-simple", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});

            // Validate that GetInitProcess fails with the process argument is null.
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER), container.Get().GetInitProcess(nullptr));
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
            WSLCContainerLauncher launcher(
                "debian:latest", "test-default-entrypoint", {"/bin/cat"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeHost, WSLCProcessFlagsStdin);

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

        // Validate that the default stop signal can be overriden.
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
            auto hr = m_defaultSession->CreateContainer(&options, &container);
            VERIFY_ARE_EQUAL(hr, E_INVALIDARG);
        }

        // Test null container name
        {
            WSLCContainerOptions options{};
            options.Image = "debian:latest";
            options.Name = nullptr;
            options.InitProcessOptions.CommandLine = {.Values = nullptr, .Count = 0};

            wil::com_ptr<IWSLCContainer> container;
            VERIFY_SUCCEEDED(m_defaultSession->CreateContainer(&options, &container));
            VERIFY_SUCCEEDED(container->Delete(WSLCDeleteFlagsNone));
        }
    }

    TEST_METHOD(ContainerStartAfterStop)
    {
        WSL2_TEST_ONLY();

        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start", {"echo", "OK"});
            auto container = launcher.Launch(*m_defaultSession);
            auto process = container.GetInitProcess();

            ValidateProcessOutput(process, {{1, "OK\n"}});

            {
                // Validate that the container can be restarted.
                VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr), S_OK);
                auto restartedProcess = container.GetInitProcess();
                ValidateProcessOutput(restartedProcess, {{1, "OK\n"}});
            }

            {
                // Validate that the container can be restarted without the attach flag.
                VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr), S_OK);
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

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            auto initProcess = container.GetInitProcess();
            initProcess.Get().Signal(WSLCSignalSIGKILL);
            VERIFY_ARE_EQUAL(initProcess.Wait(), WSLCSignalSIGKILL + 128);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));

            // Validate that deleted containers can't be started.
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr), RPC_E_DISCONNECTED);
        }

        // Validate restart behavior for a container with the autorm flag set
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start-3", {"sleep", "99999"});
            launcher.SetContainerFlags(WSLCContainerFlagsRm);
            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            // Validate that deleted containers can't be started.
            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsNone, nullptr), RPC_E_DISCONNECTED);
        }

        // Validate that invalid start flags are rejected.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-stop-start-invalid-flags", {"echo", "OK"});
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.Get().Start(static_cast<WSLCContainerStartFlags>(0x2), nullptr), E_INVALIDARG);
        }
    }

    TEST_METHOD(OpenContainer)
    {
        WSL2_TEST_ONLY();

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
            expectOpen("", E_INVALIDARG);
            ValidateCOMErrorMessage(L"Invalid name: ''");

            expectOpen("non-existing-container", HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

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

    TEST_METHOD(ContainerState)
    {
        WSL2_TEST_ONLY();

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLCContainerState>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;

            VERIFY_SUCCEEDED(
                m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                    &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(m_defaultSession->ListContainers(
                    &containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
            VERIFY_ARE_EQUAL(container.Get().Stop(WSLCSignalSIGKILL, 0), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that the container is in running state.
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGTERM, 0));

            expectContainerList({{"test-container-2", "debian:latest", WslcContainerStateExited}});

            // Verify that the container is in exited state.
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Verify that deleting a container stopped via Stop() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            expectContainerList({});
        }

        // Validate that Kill() works as expected
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-kill", {"sleep", "99999"}, {});

            auto container = launcher.Create(*m_defaultSession);

            // Validate that a created container cannot be killed.
            VERIFY_ARE_EQUAL(container.Get().Kill(WSLCSignalNone), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalNone));

            // Verify that the container is in exited state.
            expectContainerList({{"test-container-kill", "debian:latest", WslcContainerStateExited}});

            // Validate that killing a non-running container fails (unlike Stop())
            VERIFY_ARE_EQUAL(container.Get().Kill(WSLCSignalNone), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that deleting a container stopped via Kill() works.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsNone));
            expectContainerList({});
        }

        // Validate that Kill() works with non-sigkill signals.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-container-kill-2", {"sleep", "99999"}, {});
            launcher.SetContainerFlags(WSLCContainerFlagsInit);

            auto container = launcher.Create(*m_defaultSession);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Kill(WSLCSignalSIGTERM));

            VERIFY_ARE_EQUAL(container.GetInitProcess().Wait(120 * 1000), WSLCSignalSIGTERM + 128);

            // Verify that the container is in exited state.
            expectContainerList({{"test-container-kill-2", "debian:latest", WslcContainerStateExited}});
        }

        // Verify that trying to open a non existing container fails.
        {
            wil::com_ptr<IWSLCContainer> sameContainer;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("does-not-exist", &sameContainer), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Validate that container names are unique.
        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-unique-name", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeHost);

            auto container = launcher.Launch(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Validate that a container with the same name cannot be started
            VERIFY_ARE_EQUAL(
                WSLCContainerLauncher("debian:latest", "test-unique-name", {"echo", "OK"}).LaunchNoThrow(*m_defaultSession).first,
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that running containers can't be deleted.
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

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
            WSLCContainerLauncher otherLauncher(
                "debian:latest", "test-unique-name", {"echo", "OK"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeHost);

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
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsNone, nullptr));

            // Verify that Start() can't be called again on a running container.
            VERIFY_ARE_EQUAL(container->Get().Start(WSLCContainerStartFlagsNone, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

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

    TEST_METHOD(DeleteContainer)
    {
        WSL2_TEST_ONLY();
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
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Verify that a running container can be deleted with the force flag.
            VERIFY_SUCCEEDED(container.Get().Delete(WSLCDeleteFlagsForce));
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsForce), HRESULT_FROM_WIN32(RPC_E_DISCONNECTED));

            // Validate that invalid flags are rejected.
            VERIFY_ARE_EQUAL(container.Get().Delete(static_cast<WSLCDeleteFlags>(0x2)), E_INVALIDARG);
        }
    }

    TEST_METHOD(ContainerNetwork)
    {
        WSL2_TEST_ONLY();

        auto expectContainerList = [&](const std::vector<std::tuple<std::string, std::string, WSLCContainerState>>& expectedContainers) {
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;

            VERIFY_SUCCEEDED(
                m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
            WSLCContainerLauncher launcher(
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeHost);

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
            WSLCContainerLauncher launcher(
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeNone);

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
            WSLCContainerLauncher launcher(
                "debian:latest",
                "test-network",
                {"sleep", "99999"},
                {},
                (WSLCContainerNetworkType)6 // WSLCContainerNetworkType::WSLCContainerNetworkTypeNone
            );

            auto retVal = launcher.LaunchNoThrow(*m_defaultSession);
            VERIFY_ARE_EQUAL(retVal.first, E_INVALIDARG);
        }

        {
            WSLCContainerLauncher launcher(
                "debian:latest", "test-network", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeBridged);

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

    TEST_METHOD(ContainerInspect)
    {
        WSL2_TEST_ONLY();

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

            WSLCContainerLauncher launcher(
                "debian:latest", "test-container-inspect", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeBridged);

            launcher.AddPort(1234, 8000, AF_INET);
            launcher.AddPort(1235, 8000, AF_INET);
            launcher.AddPort(1236, 8001, AF_INET);
            launcher.AddVolume(testFolder.wstring(), "/test-volume", false);
            launcher.AddVolume(testFolderReadOnly.wstring(), "/test-volume-ro", true);
            launcher.AddTmpfs("/mnt/wslc-tmpfs-inspect", "");

            auto container = launcher.Launch(*m_defaultSession);
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
    }

    TEST_METHOD(Exec)
    {
        WSL2_TEST_ONLY();

        // Create a container.
        WSLCContainerLauncher launcher(
            "debian:latest", "test-container-exec", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeNone);

        auto container = launcher.Launch(*m_defaultSession);

        // Simple exec case.
        {
            auto process = WSLCProcessLauncher({}, {"echo", "OK"}).Launch(container.Get());

            ValidateProcessOutput(process, {{1, "OK\n"}});
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
            auto [result, _] = WSLCProcessLauncher({}, {"/bin/cat"}).LaunchNoThrow(container.Get());
            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    TEST_METHOD(ExecContainerDelete)
    {
        WSL2_TEST_ONLY();

        WSLCContainerLauncher launcher("debian:latest", "test-exec-dtor", {"sleep", "99999"}, {}, WSLCContainerNetworkType::WSLCContainerNetworkTypeNone);

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

    void RunPortMappingsTest(IWSLCSession& session, WSLCContainerNetworkType containerNetworkType)
    {
        LogInfo("Container network type: %d", static_cast<int>(containerNetworkType));

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
                VERIFY_SUCCEEDED(session.ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

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
                VERIFY_SUCCEEDED(session.ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

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
            VERIFY_ARE_EQUAL(hresult, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Verify that a stopped container returns no ports.
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            {
                wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
                wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
                VERIFY_SUCCEEDED(session.ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

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

            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
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
            VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEACCES));

            // Validate that Create() correctly cleans up bound ports after a port fails to map
            {
                WSLCContainerLauncher launcher(
                    "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, containerNetworkType);
                launcher.AddPort(1236, 8000, AF_INET); // Should succeed
                launcher.AddPort(1235, 8000, AF_INET); // Should fail.

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(WSAEACCES));

                // Validate that port 1234 is still available.
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
                options.ContainerNetwork.ContainerNetworkType = containerNetworkType;

                wil::com_ptr<IWSLCContainer> container;
                VERIFY_ARE_EQUAL(session.CreateContainer(&options, &container), E_INVALIDARG);
            }

            // TODO: Update once UDP is supported.
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, IPPROTO_UDP);

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            }

            // TODO: Update once custom binding addresses are supported.
            {
                WSLCContainerLauncher launcher("python:3.12-alpine", {}, {}, {}, containerNetworkType);
                launcher.AddPort(1234, 8000, AF_INET, IPPROTO_TCP, "1.1.1.1");

                VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(session).first, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
            }
        }
    }

    auto SetupPortMappingsTest(WSLCNetworkingMode networkingMode)
    {
        auto settings = GetDefaultSessionSettings(L"networking-session", true, networkingMode);

        auto createNewSession = settings.NetworkingMode != m_defaultSessionSettings.NetworkingMode;
        auto restore = createNewSession ? std::optional{ResetTestSession()} : std::nullopt;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        return std::make_pair(std::move(restore), std::move(session));
    }

    TEST_METHOD(PortMappingsNat)
    {
        WSL2_TEST_ONLY();

        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeNAT);

        RunPortMappingsTest(*session, WSLCContainerNetworkTypeBridged);
        RunPortMappingsTest(*session, WSLCContainerNetworkTypeHost);
    }

    TEST_METHOD(PortMappingsVirtioProxy)
    {
        WSL2_TEST_ONLY();

        auto [restore, session] = SetupPortMappingsTest(WSLCNetworkingModeVirtioProxy);

        RunPortMappingsTest(*session, WSLCContainerNetworkTypeBridged);
        RunPortMappingsTest(*session, WSLCContainerNetworkTypeHost);
    }

    TEST_METHOD(PortMappingsNone)
    {
        // Validate that trying to map ports without network fails.
        WSLCContainerLauncher launcher(
            "python:3.12-alpine", "test-ports-fail", {"python3", "-m", "http.server"}, {"PYTHONUNBUFFERED=1"}, WSLCContainerNetworkTypeNone);

        launcher.AddPort(1234, 8000, AF_INET);

        VERIFY_ARE_EQUAL(launcher.LaunchNoThrow(*m_defaultSession).first, E_INVALIDARG);
    }

    void ValidateContainerVolumes(bool enableVirtioFs)
    {
        WSL2_TEST_ONLY();

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

    void ValidateContainerVolumeUnmountAllFoldersOnError(bool enableVirtioFs)
    {
        WSL2_TEST_ONLY();

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

            wsl::windows::common::relay::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::relay::LineBasedReadHandle>(std::move(readPipe), std::move(onData), Crlf));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::relay::WriteHandle>(std::move(writePipe), buffer));

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

            wsl::windows::common::relay::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::relay::HTTPChunkBasedReadHandle>(std::move(readPipe), std::move(onData)));

            std::vector<char> buffer{Data.begin(), Data.end()};
            io.AddHandle(std::make_unique<wsl::windows::common::relay::WriteHandle>(std::move(writePipe), buffer));

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

            auto reader = std::make_unique<wsl::windows::common::relay::HTTPChunkBasedReadHandle>(
                wsl::windows::common::relay::HandleWrapper{nullptr}, std::move(onData));

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

    TEST_METHOD(WriteHandleContent)
    {
        WSL2_TEST_ONLY();

        // Validate that writing to a pipe works as expected.
        {
            const std::string expectedData = "Pipe-test";
            std::vector<char> writeBuffer{expectedData.begin(), expectedData.end()};

            auto [readPipe, writePipe] = wsl::windows::common::wslutil::OpenAnonymousPipe(16 * 1024, true, false);

            std::string readData;
            wsl::windows::common::relay::MultiHandleWait io;

            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(std::move(readPipe), [&](const gsl::span<char>& buffer) {
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

            wsl::windows::common::relay::MultiHandleWait io;
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
    }

    TEST_METHOD(DockerIORelay)
    {
        using namespace wsl::windows::common::relay;

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
    }

    TEST_METHOD(ContainerRecoveryFromStorage)
    {
        WSL2_TEST_ONLY();

        auto restore = ResetTestSession(); // Required to access the storage folder.

        std::string containerName = "test-container";
        ULONGLONG originalStateChangedAt{};
        ULONGLONG originalCreatedAt{};

        // Phase 1: Create session and container, then stop the container
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Create and start a container
            WSLCContainerLauncher launcher("debian:latest", containerName.c_str(), {"/bin/echo", "OK"});

            auto container = launcher.Launch(*session);
            container.SetDeleteOnClose(false);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Stop the container so it can be recovered and deleted later
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateExited);

            // Capture StateChangedAt and CreatedAt before the session is destroyed.
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(session->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(session->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
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
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }

        // Phase 3: Create new session from same storage, verify the container is not listed.
        {
            auto session = CreateSession(GetDefaultSessionSettings(L"recovery-test", true));

            // Verify container is no longer accessible
            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(session->OpenContainer(containerName.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        }
    }

    TEST_METHOD(ContainerVolumeAndPortRecoveryFromStorage)
    {
        WSL2_TEST_ONLY();

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
                "python:3.12-alpine", containerName, {"python3", "-m", "http.server", "--directory", "/volume"}, {"PYTHONUNBUFFERED=1"}, WSLCContainerNetworkTypeBridged);

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
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, nullptr));

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

        VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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

    TEST_METHOD(SessionManagement)
    {
        WSL2_TEST_ONLY();

        auto manager = OpenSessionManager();

        auto expectSessions = [&](const std::vector<std::wstring>& expectedSessions) {
            wil::unique_cotaskmem_array_ptr<WSLCSessionInformation> sessions;
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
            VERIFY_ARE_EQUAL(manager->CreateSession(&settings, WSLCSessionFlagsPersistent, &session), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

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

    TEST_METHOD(ContainerLogs)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(ContainerLabels)
    {
        WSL2_TEST_ONLY();

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
            auto hr = m_defaultSession->CreateContainer(&options, &container);
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
            auto hr = m_defaultSession->CreateContainer(&options, &container);
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
            auto hr = m_defaultSession->CreateContainer(&options, &container);
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

    TEST_METHOD(ContainerAttach)
    {
        WSL2_TEST_ONLY();

        // Validate attach behavior in a non-tty process.
        {
            WSLCContainerLauncher launcher("debian:latest", "attach-test-1", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);

            // Verify that attaching to a created container fails.
            COMOutputHandle stdinHandle{};
            COMOutputHandle stdoutHandle{};
            COMOutputHandle stderrHandle{};
            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

            // Start the container.
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsAttach, nullptr));

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
            VERIFY_ARE_EQUAL(container->Get().Attach(nullptr, &stdinHandle, &stdoutHandle, &stderrHandle), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));

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

    TEST_METHOD(InvalidNames)
    {
        WSL2_TEST_ONLY();

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
            VERIFY_ARE_EQUAL(m_defaultSession->PullImage(name, nullptr, nullptr), E_INVALIDARG);

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

    TEST_METHOD(PageReporting)
    {
        WSL2_TEST_ONLY();
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

    TEST_METHOD(ContainerAutoRemove)
    {
        WSL2_TEST_ONLY();

        // Test that a container with the Rm flag is automatically deleted on Stop().
        {
            WSLCContainerLauncher launcher("debian:latest", "test-auto-remove", {"/bin/cat"}, {}, {}, WSLCProcessFlagsStdin);
            launcher.SetContainerFlags(WSLCContainerFlagsRm);

            auto container = launcher.Launch(*m_defaultSession);

            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove-kill", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            // verifyContainerDeleted("test-auto-remove");
            VERIFY_ARE_EQUAL(container.Get().Delete(WSLCDeleteFlagsNone), RPC_E_DISCONNECTED);

            wil::com_ptr<IWSLCContainer> notFound;
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(id.c_str(), &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(
                m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
            VERIFY_ARE_EQUAL(containers.size(), 0);
        }
    }

    TEST_METHOD(ContainerAutoRemoveReadStdout)
    {
        WSL2_TEST_ONLY();

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
        VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer("test-auto-remove-stdout", &notFound), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

        wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
        wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
        VERIFY_SUCCEEDED(m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));
        VERIFY_ARE_EQUAL(containers.size(), 0);
    }

    TEST_METHOD(ContainerNameGeneration)
    {
        WSL2_TEST_ONLY();

        {
            // Create a container with a specific name
            auto container = WSLCContainerLauncher("debian:latest", "test-container-name").Create(*m_defaultSession.get());

            // Validate that the container name is correct.
            VERIFY_ARE_EQUAL(container.Name(), "test-container-name");
        }

        {
            // Create a container without name.
            auto container = WSLCContainerLauncher("debian:latest").Create(*m_defaultSession.get());

            // Validate that the service generates a name for the container.
            VERIFY_ARE_NOT_EQUAL(container.Name(), "");
        }
    }

    TEST_METHOD(DeferredPortAndVolumeMappingOnStart)
    {
        WSL2_TEST_ONLY();

        // Verify port mapping.
        // Two containers created with the same host port, only the first Start() succeeds.
        {
            WSLCContainerLauncher launcher("debian:latest", "deferred-port", {"sleep", "99999"}, {}, WSLCContainerNetworkTypeBridged);
            launcher.AddPort(1240, 8000, AF_INET);

            // Both Create() calls should succeed because ports are not reserved until Start().
            auto container = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateCreated);

            launcher.SetName("deferred-port-2");
            auto container2 = launcher.Create(*m_defaultSession);
            VERIFY_ARE_EQUAL(container2.State(), WslcContainerStateCreated);

            // Start container — should succeed.
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container.State(), WslcContainerStateRunning);

            // Start container 2 — should fail because the host port is already reserved by container 1.
            VERIFY_ARE_EQUAL(container2.Get().Start(WSLCContainerStartFlagsNone, nullptr), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
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

            WSLCContainerLauncher launcher("debian:latest", "deferred-volume", {"sleep", "99999"}, {}, WSLCContainerNetworkTypeHost);
            launcher.AddVolume(hostFolder.wstring(), "/deferred-volume", false);

            // Create the container — volume should NOT be mounted yet.
            auto [result, container] = launcher.CreateNoThrow(*m_defaultSession);
            VERIFY_SUCCEEDED(result);
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateCreated);
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount);

            // Start the container — volume should now be mounted.
            VERIFY_SUCCEEDED(container->Get().Start(WSLCContainerStartFlagsNone, nullptr));
            VERIFY_ARE_EQUAL(container->State(), WslcContainerStateRunning);
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount + 1);

            // Verify the volume is unmounted after container is stopped.
            VERIFY_SUCCEEDED(container->Get().Stop(WSLCSignalSIGKILL, 0));
            VERIFY_ARE_EQUAL(getMountCount(), baselineMountCount);
        }
    }

    // This test case validates that multiple operations can happen in parallel in the same session.
    TEST_METHOD(ParallelSessionOperations)
    {
        WSL2_TEST_ONLY();

        // Start a blocking export
        BlockingOperation operation([&](HANDLE handle) {
            return m_defaultSession->SaveImage(ToCOMInputHandle(handle), "debian:latest", nullptr, nullptr);
        });

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { operation.Complete(); });

        // Validate that various operations can be done while the export is in progress.

        {
            wil::unique_cotaskmem_array_ptr<WSLCContainerEntry> containers;
            wil::unique_cotaskmem_array_ptr<WSLCContainerPortMapping> ports;
            VERIFY_SUCCEEDED(
                m_defaultSession->ListContainers(&containers, containers.size_address<ULONG>(), &ports, ports.size_address<ULONG>()));

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

    TEST_METHOD(ParallelContainerOperations)
    {
        WSL2_TEST_ONLY();

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
            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        }
    }

    TEST_METHOD(SessionTerminationDuringSave)
    {
        WSL2_TEST_ONLY();

        // Validate that SaveImage is aborted when the session terminates.
        // Use overlapped write pipe so the server-side WriteFile doesn't block synchronously.
        BlockingOperation operation(
            [&](HANDLE handle) { return m_defaultSession->SaveImage(ToCOMInputHandle(handle), "debian:latest", nullptr, nullptr); }, E_ABORT, true, true);

        // Terminate the session.
        VERIFY_SUCCEEDED(m_defaultSession->Terminate());
        operation.Complete();
        auto restore = ResetTestSession();
    }

    TEST_METHOD(SessionTerminationDuringExport)
    {
        WSL2_TEST_ONLY();

        // Validate that container Export is aborted when the session terminates.
        WSLCContainerLauncher launcher("debian:latest", "test-export-session-terminate", {"echo", "OK"});
        auto container = launcher.Launch(*m_defaultSession);
        VERIFY_ARE_EQUAL(container.GetInitProcess().Wait(), 0);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            PruneResult result;
            LOG_IF_FAILED(m_defaultSession->PruneContainers(nullptr, 0, 0, &result.result));
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

    TEST_METHOD(InteractiveDetach)
    {
        WSL2_TEST_ONLY();

        auto validateDetaches = [](HANDLE TtyIn, HANDLE TtyOut, const std::vector<char>& Input) {
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(TtyIn, Input.data(), static_cast<DWORD>(Input.size()), nullptr, nullptr));

            std::string output;
            auto onRead = [&](const gsl::span<char>& data) { output.append(data.data(), data.size()); };

            wsl::windows::common::relay::MultiHandleWait io;
            io.AddHandle(std::make_unique<wsl::windows::common::relay::ReadHandle>(TtyOut, std::move(onRead)));

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
            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsAttach, DetachKeys));

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

            VERIFY_ARE_EQUAL(container.Get().Start(WSLCContainerStartFlagsAttach, "invalid"), E_INVALIDARG);

            VERIFY_SUCCEEDED(container.Get().Start(WSLCContainerStartFlagsNone, nullptr));

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

    TEST_METHOD(ContainerPrune)
    {
        WSL2_TEST_ONLY();

        auto expectPrune = [this](
                               const std::vector<std::string>& expectedIds = {},
                               const std::map<std::string, std::pair<const char*, bool>>& labels = {},
                               uint64_t until = 0,
                               const std::source_location& source = std::source_location::current()) {
            PruneResult result;

            std::vector<WSLCPruneLabelFilter> labelsFilter;
            for (const auto& e : labels)
            {
                labelsFilter.push_back({e.first.c_str(), e.second.first, e.second.second});
            }

            VERIFY_SUCCEEDED(m_defaultSession->PruneContainers(
                labels.empty() ? nullptr : labelsFilter.data(), static_cast<DWORD>(labelsFilter.size()), until, &result.result));

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
            VERIFY_ARE_EQUAL(m_defaultSession->OpenContainer(containerId.c_str(), &dummy), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

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
            expectPrune({testPrune1.Id()}, {{"key", {"value", true}}});

            // Expect testPrune2 to be selected via key being present.
            expectPrune({testPrune2.Id()}, {{"key", {nullptr, true}}});

            // Prune by absence of 'anotherKey' label.
            expectPrune({testPrune4.Id()}, {{"anotherKey", {nullptr, false}}});

            // Prune by label inequality.
            expectPrune({testPrune3.Id()}, {{"anotherKey", {"someValue", false}}});
        }

        // Validate that the 'until' filter works.
        {
            WSLCContainerLauncher launcher("debian:latest", "test-prune-until", {"echo", "OK"}, {}, {});

            auto container = RunAndWait(launcher);

            auto now = time(nullptr);

            expectPrune({}, {}, now - 3600);
            expectPrune({container.Id()}, {}, now + 3600);
        }

        // Validate error paths.
        {
            WSLCPruneLabelFilter filter{.Key = nullptr, .Value = nullptr, .Present = false};
            PruneResult result;

            VERIFY_ARE_EQUAL(m_defaultSession->PruneContainers(&filter, 1, 0, &result.result), E_POINTER);
            VERIFY_ARE_EQUAL(m_defaultSession->PruneContainers(&filter, 1, 0, nullptr), HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));
        }
    }

    TEST_METHOD(ImagePrune)
    {
        WSL2_TEST_ONLY();

        auto pruneImages =
            [this](DWORD flags = WSLCPruneImagesFlagsNone, uint64_t until = 0, const std::vector<WSLCPruneLabelFilter>& labels = {}) {
                wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
                ULONGLONG spaceReclaimed = 0;
                WSLCPruneImagesOptions options{};
                options.Flags = flags;
                options.Until = until;
                options.Labels = labels.empty() ? nullptr : labels.data();
                options.LabelsCount = static_cast<ULONG>(labels.size());

                VERIFY_SUCCEEDED(m_defaultSession->PruneImages(
                    &options, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed));
                return std::make_pair(std::move(deletedImages), spaceReclaimed);
            };

        // Helper to create a dangling image: load alpine, then retag it to a temp name
        // so the original alpine image ID becomes dangling.
        auto createDanglingAlpine = [this]() {
            LoadTestImage("alpine:latest");
            WSLCTagImageOptions tagOptions{.Image = "alpine:latest", .Repo = "alpine-prune-temp", .Tag = "latest"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&tagOptions));
            // Remove the original tag, leaving the image dangling if it has a different ID than the temp tag.
            // Actually: tag alpine:latest with a temp name, then overwrite alpine:latest with debian to make the original dangling.
            WSLCTagImageOptions overwriteOptions{.Image = "debian:latest", .Repo = "alpine", .Tag = "latest"};
            VERIFY_SUCCEEDED(m_defaultSession->TagImage(&overwriteOptions));
        };

        auto cleanupAlpine = [this, &pruneImages]() {
            pruneImages(WSLCPruneImagesFlagsDanglingTrue);
            LOG_IF_FAILED(DeleteImageNoThrow("alpine:latest", WSLCDeleteImageFlagsNone).first);
            LOG_IF_FAILED(DeleteImageNoThrow("alpine-prune-temp:latest", WSLCDeleteImageFlagsNone).first);
        };

        // Clean up any stale dangling images from prior tests.
        pruneImages(WSLCPruneImagesFlagsDanglingTrue);

        // Prune with no unused images returns empty.
        {
            auto [deletedImages, spaceReclaimed] = pruneImages();
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);
        }

        // Validate dangling prune: create a dangling image by re-tagging, then prune it.
        {
            createDanglingAlpine();
            auto cleanup = wil::scope_exit([&]() { cleanupAlpine(); });

            // DanglingTrue should prune the now-dangling original alpine image.
            auto [deletedImages, spaceReclaimed] = pruneImages(WSLCPruneImagesFlagsDanglingTrue);
            VERIFY_IS_TRUE(deletedImages.size() > 0);

            // A second prune should find nothing.
            auto [deletedImages2, spaceReclaimed2] = pruneImages(WSLCPruneImagesFlagsDanglingTrue);
            VERIFY_ARE_EQUAL(deletedImages2.size(), 0u);
        }

        // Validate 'until' filter.
        {
            createDanglingAlpine();
            auto cleanup = wil::scope_exit([&]() { cleanupAlpine(); });

            // Docker's 'until' filter uses the image's original Created timestamp, not load time.
            // Use timestamp 1 (near epoch) which is before any real image was built.
            auto [deletedImages, spaceReclaimed] = pruneImages(WSLCPruneImagesFlagsNone, 1);
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);

            // Use a timestamp far in the future to ensure the dangling image is pruned.
            auto future = static_cast<uint64_t>(time(nullptr)) + 3600;
            auto [deletedImages2, spaceReclaimed2] = pruneImages(WSLCPruneImagesFlagsNone, future);
            VERIFY_IS_TRUE(deletedImages2.size() > 0);
        }

        // Validate label filters.
        {
            createDanglingAlpine();
            auto cleanup = wil::scope_exit([&]() { cleanupAlpine(); });

            // Prune with a label filter that no dangling image has - should not prune anything.
            auto [deletedImages, spaceReclaimed] =
                pruneImages(WSLCPruneImagesFlagsNone, 0, {{.Key = "nonexistent.label", .Value = nullptr, .Present = true}});
            VERIFY_ARE_EQUAL(deletedImages.size(), 0u);

            // Prune with absent label filter - dangling image doesn't have the label, so it matches.
            auto [deletedImages2, spaceReclaimed2] =
                pruneImages(WSLCPruneImagesFlagsNone, 0, {{.Key = "nonexistent.label", .Value = nullptr, .Present = false}});
            VERIFY_IS_TRUE(deletedImages2.size() > 0);
        }

        // Validate null Options uses defaults (dangling-only prune).
        {
            LoadTestImage("alpine:latest");
            auto cleanup = wil::scope_exit([&]() { cleanupAlpine(); });

            ExpectImagePresent(*m_defaultSession, "alpine:latest");

            // Null options should not prune tagged images.
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            ULONGLONG spaceReclaimed = 0;
            VERIFY_SUCCEEDED(m_defaultSession->PruneImages(nullptr, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed));
            ExpectImagePresent(*m_defaultSession, "alpine:latest");
        }

        // Validate error paths.
        {
            // Null output pointers - RPC rejects null [out] pointers before our code runs.
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            ULONGLONG spaceReclaimed = 0;
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(nullptr, nullptr, deletedImages.size_address<ULONG>(), &spaceReclaimed),
                HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER));

            // Mutually exclusive dangling flags.
            WSLCPruneImagesOptions invalidOptions{};
            invalidOptions.Flags = WSLCPruneImagesFlagsDanglingTrue | WSLCPruneImagesFlagsDanglingFalse;
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(&invalidOptions, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed),
                E_INVALIDARG);

            // Invalid flags.
            invalidOptions.Flags = 0x4;
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(&invalidOptions, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed),
                E_INVALIDARG);

            // Null label key.
            WSLCPruneLabelFilter nullKeyFilter{.Key = nullptr, .Value = nullptr, .Present = false};
            invalidOptions.Flags = WSLCPruneImagesFlagsNone;
            invalidOptions.Labels = &nullKeyFilter;
            invalidOptions.LabelsCount = 1;
            VERIFY_ARE_EQUAL(
                m_defaultSession->PruneImages(&invalidOptions, deletedImages.addressof(), deletedImages.size_address<ULONG>(), &spaceReclaimed),
                E_INVALIDARG);
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

    TEST_METHOD(ElevatedTokenCanOpenNonElevatedHandles)
    {
        WSL2_TEST_ONLY();

        wil::com_ptr<IWSLCSession> nonElevatedSession;

        {
            auto nonElevatedToken = GetNonElevatedToken(TokenImpersonation);
            auto revert = wil::impersonate_token(nonElevatedToken.get());

            nonElevatedSession = CreateSession(GetDefaultSessionSettings(L"non-elevated-session"), WSLCSessionFlagsNone);
            LoadTestImage("debian:latest", nonElevatedSession.get());

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
};
