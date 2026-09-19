/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCImageTests.cpp

Abstract:

    This file contains test cases for the WSLC image pull, load, list and push API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCImageTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCImageTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    // Unhide the helpers shadowed by the test methods of the same name.
    using WSLCTestBase::DeleteImage;

    std::string PushImageToRegistry(const std::string& imageName, const std::string& registryAddress, const std::string& registryAuth)
    {
        auto reference = ImageReference::Parse(imageName);
        const auto& repo = reference.Repository.Name;
        auto tag = reference.TagOrDigest();
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

        VERIFY_SUCCEEDED(m_defaultSession->PushImage(registryImage.c_str(), registryAuth.c_str(), FALSE, nullptr, nullptr));

        return registryImage;
    }

    WSLC_TEST_METHOD(PullImage)
    {
        {
            // Start a local registry without auth and push hello-world:latest to it.
            auto [registryContainer, registryAddress] = StartLocalRegistry(*m_defaultSession);

            auto image = PushImageToRegistry("hello-world:latest", registryAddress, BuildRegistryAuthHeader("", ""));
            ExpectImagePresent(*m_defaultSession, image.c_str(), false);

            VERIFY_SUCCEEDED(m_defaultSession->PullImage(image.c_str(), nullptr, FALSE, nullptr, nullptr));
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

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("does-not:exist", nullptr, FALSE, nullptr, nullptr), WSLC_E_IMAGE_NOT_FOUND);
            ValidateCOMErrorMessage(expectedError.c_str());
        }

        // Validate that PullImage() returns the appropriate error if the session is terminated.
        {
            VERIFY_SUCCEEDED(m_defaultSession->Terminate());

            auto cleanup = wil::scope_exit([&]() {
                ResetTestSession(); // Reopen the test session since the session was terminated.
            });

            VERIFY_ARE_EQUAL(m_defaultSession->PullImage("hello-world:linux", nullptr, FALSE, nullptr, nullptr), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
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

            VERIFY_SUCCEEDED(m_defaultSession->PullImage(registryImage.c_str(), nullptr, FALSE, nullptr, nullptr));

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
            VERIFY_SUCCEEDED(m_defaultSession->PullImage(Image.c_str(), nullptr, FALSE, nullptr, nullptr));

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

            VERIFY_ARE_EQUAL(session->PullImage("pytorch/pytorch", nullptr, FALSE, nullptr, nullptr), E_FAIL);

            ValidateCOMErrorMessageContains(L"no space left on device");
        }
    }

    WSLC_TEST_METHOD(PushImage)
    {
        auto emptyAuth = BuildRegistryAuthHeader("", "");

        // Validate that pushing a non-existent image fails.
        {
            VERIFY_ARE_EQUAL(m_defaultSession->PushImage("does-not-exist:latest", emptyAuth.c_str(), FALSE, nullptr, nullptr), E_FAIL);
            ValidateCOMErrorMessage(L"An image does not exist locally with the tag: does-not-exist");
        }

        // Validate passing empty auth string returns an appropriate error.
        {
            VERIFY_ARE_EQUAL(m_defaultSession->PushImage("does-not-exist:latest", "", FALSE, nullptr, nullptr), E_INVALIDARG);
        }

        // Validate that PushImage() returns the appropriate error if the session is terminated.
        {
            VERIFY_SUCCEEDED(m_defaultSession->Terminate());
            auto cleanup = wil::scope_exit([&]() { ResetTestSession(); });

            VERIFY_ARE_EQUAL(
                m_defaultSession->PushImage("hello-world:latest", emptyAuth.c_str(), FALSE, nullptr, nullptr),
                HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
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
        VERIFY_ARE_EQUAL(m_defaultSession->PullImage(image.c_str(), nullptr, FALSE, nullptr, nullptr), E_FAIL);
        ValidateCOMErrorMessageContains(L"no basic auth credentials");

        // Pulling with credentials should succeed.
        VERIFY_SUCCEEDED(m_defaultSession->PullImage(image.c_str(), xRegistryAuth.c_str(), FALSE, nullptr, nullptr));
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

    // Loading the same image tar repeatedly must not permanently grow the session storage VHD.
    //
    // Docker's /images/load handler extracts the incoming tar into a temporary directory under its data
    // root (/var/lib/docker, which is the storage VHD) before inspecting any digest, so every call
    // writes roughly one tar's worth of data to the VHD even when the layers already exist and are
    // deduplicated. That temporary directory is deleted afterwards, but the VHD is a non-sparse
    // dynamically expanding VHDX mounted without 'discard', so the freed blocks are never returned to
    // the host, and ext4 tends to satisfy the next extraction from a different region rather than
    // reusing the just-freed one. The result is a VHD that grows by about the tar size on every load.
    //
    // The size of the tar matters: a small tar is re-extracted into blocks the VHDX has already
    // allocated, so the growth plateaus immediately and the bug does not reproduce. This test therefore
    // builds a large image with incompressible layers (which is what a real from-source application
    // image looks like once 'save' has written its uncompressed layers out) rather than reusing one of
    // the small prebuilt test tars.
    WSLC_TEST_METHOD(LoadImageRepeatedDoesNotGrowStorageVhd)
    {
        SKIP_TEST_SERVER();

        constexpr auto c_sessionName = L"wslc-load-image-vhd-growth";
        constexpr auto c_imageName = "wslc-test-load-growth:latest";
        constexpr auto c_layerCount = 4;
        constexpr auto c_layerSizeMb = 256;
        constexpr auto c_extraLoads = 3;

        // Build the image in the shared session (it already has debian:latest), then export it. Each
        // layer is /dev/urandom so it cannot be compressed away, making the exported tar's size
        // representative of the data the engine has to move on every load.
        //
        // Pass /p:LoadGrowthTar=<path> to run the loop against an existing tar (for example one produced
        // by 'wslc build' + 'wslc save' for a real application image) instead of building one here.
        WEX::Common::String existingTar;
        WEX::TestExecution::RuntimeParameters::TryGetValue(L"LoadGrowthTar", existingTar);
        const bool useExistingTar = !existingTar.IsEmpty();

        auto contextDir = std::filesystem::current_path() / "build-context-load-growth";
        const auto imageTar = useExistingTar ? std::filesystem::path{static_cast<LPCWSTR>(existingTar)}
                                             : std::filesystem::current_path() / "wslc-load-growth.tar";

        auto buildCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (useExistingTar)
            {
                return;
            }

            LOG_IF_FAILED(DeleteImageNoThrow(c_imageName, WSLCDeleteImageFlagsForce).first);

            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
            std::filesystem::remove(imageTar, ec);
        });

        if (!useExistingTar)
        {
            std::filesystem::create_directories(contextDir);

            {
                std::ofstream dockerfile(contextDir / "Dockerfile");
                dockerfile << "FROM debian:latest\n";
                for (auto i = 0; i < c_layerCount; i++)
                {
                    dockerfile << std::format("RUN dd if=/dev/urandom bs=1M count={} of=/blob{}.bin status=none\n", c_layerSizeMb, i);
                }
            }

            VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, c_imageName));

            wil::unique_handle tarFile{CreateFileW(
                imageTar.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());
            VERIFY_SUCCEEDED(m_defaultSession->SaveImage(ToCOMInputHandle(tarFile.get()), c_imageName, nullptr, nullptr));
        }

        const auto tarSize = static_cast<uint64_t>(std::filesystem::file_size(imageTar));
        VERIFY_IS_TRUE(tarSize > 0);

        // A dedicated storage directory is required: the shared class storage is preloaded with the test
        // images and is written to by every other test in this class, so its size says nothing here.
        const auto storageDir = std::filesystem::current_path() / "test-storage-load-image-growth";
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

        const auto vhdPath = storageDir / wsl::windows::wslc::DefaultStorageVhdName;

        // Size on disk, which is what grows as the dynamically expanding VHDX allocates blocks.
        auto vhdSizeOnDisk = [&]() {
            DWORD highPart{};
            SetLastError(NO_ERROR);
            const auto lowPart = GetCompressedFileSizeW(vhdPath.c_str(), &highPart);
            THROW_LAST_ERROR_IF(lowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR);

            ULARGE_INTEGER size{};
            size.LowPart = lowPart;
            size.HighPart = highPart;
            return static_cast<uint64_t>(size.QuadPart);
        };

        // Bytes used by the guest filesystem backing the docker data root.
        auto guestUsedBytes = [&]() {
            const auto result =
                ExpectCommandResult(session.get(), {"/bin/sh", "-c", "df -k /var/lib/docker | awk 'NR == 2 {print $3}'"}, 0);

            return std::stoull(result.Output.at(1)) * 1024;
        };

        // Entries left behind in the directory docker extracts the incoming tar into.
        auto guestTempEntryCount = [&]() {
            const auto result =
                ExpectCommandResult(session.get(), {"/bin/sh", "-c", "ls -A /var/lib/docker/tmp 2>/dev/null | wc -l"}, 0);

            return std::stoull(result.Output.at(1));
        };

        auto loadImage = [&]() {
            wil::unique_handle tarFile{
                CreateFileW(imageTar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            VERIFY_IS_FALSE(INVALID_HANDLE_VALUE == tarFile.get());

            LARGE_INTEGER fileSize{};
            VERIFY_IS_TRUE(GetFileSizeEx(tarFile.get(), &fileSize));
            VERIFY_SUCCEEDED(session->LoadImage(ToCOMInputHandle(tarFile.get()), fileSize.QuadPart, nullptr, nullptr));

            // Flush the guest page cache so the writes have reached the VHD before it is measured.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "sync"}, 0);
        };

        // The first load legitimately grows the VHD: this is where the layers are actually registered.
        // Everything measured after it is overhead from re-loading content docker already has.
        loadImage();
        if (!useExistingTar)
        {
            ExpectImagePresent(*session, c_imageName);
        }

        const auto baselineVhdSize = vhdSizeOnDisk();
        const auto baselineGuestUsed = guestUsedBytes();

        LogInfo(
            "Tar size=%llu, baseline vhd size on disk=%llu, baseline guest used=%llu",
            static_cast<unsigned long long>(tarSize),
            static_cast<unsigned long long>(baselineVhdSize),
            static_cast<unsigned long long>(baselineGuestUsed));

        for (auto i = 0; i < c_extraLoads; i++)
        {
            loadImage();

            const auto vhdSize = vhdSizeOnDisk();
            const auto guestUsed = guestUsedBytes();

            LogInfo(
                "Load %d: vhd size on disk=%llu (+%lld), guest used=%llu (+%lld), temp entries=%llu",
                i + 1,
                static_cast<unsigned long long>(vhdSize),
                static_cast<long long>(vhdSize - baselineVhdSize),
                static_cast<unsigned long long>(guestUsed),
                static_cast<long long>(guestUsed - baselineGuestUsed),
                static_cast<unsigned long long>(guestTempEntryCount()));
        }

        const auto finalVhdSize = vhdSizeOnDisk();
        const auto finalGuestUsed = guestUsedBytes();

        // The host-side VHD must not grow by roughly one tar per load. The budget allows a single tar of
        // slack in total (with a floor so that small tars don't make this flaky), which is well under the
        // c_extraLoads * tarSize that linear growth would produce.
        constexpr uint64_t c_minimumGrowthBudget = 64ull * 1024 * 1024;
        const auto growthBudget = std::max<uint64_t>(tarSize, c_minimumGrowthBudget);

        LogInfo(
            "Storage VHD grew by %llu bytes over %d reloads of a %llu byte tar (budget=%llu). Guest usage grew by %lld bytes.",
            static_cast<unsigned long long>(finalVhdSize - baselineVhdSize),
            c_extraLoads,
            static_cast<unsigned long long>(tarSize),
            static_cast<unsigned long long>(growthBudget),
            static_cast<long long>(finalGuestUsed - baselineGuestUsed));

        // Reloading the same tar must not leave docker's extraction directory behind.
        VERIFY_ARE_EQUAL(0ull, guestTempEntryCount());

        // Docker deduplicates the identical layers, so the guest filesystem must not retain a tar's
        // worth of data per load. A failure here means the temporary extraction is being leaked inside
        // the guest rather than merely being unreclaimable on the host.
        VERIFY_IS_TRUE(finalGuestUsed < baselineGuestUsed + tarSize);

        if (finalVhdSize >= baselineVhdSize + growthBudget)
        {
            LogError("The storage VHD is growing with each load: the space is being written and then not reclaimed.");

            VERIFY_FAIL();
        }

        VERIFY_SUCCEEDED(session->Terminate());
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
};
