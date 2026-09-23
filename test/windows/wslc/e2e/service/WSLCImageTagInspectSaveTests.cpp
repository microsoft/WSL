/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCImageTagInspectSaveTests.cpp

Abstract:

    This file contains test cases for the WSLC image tag, inspect and save API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCImageTagInspectSaveTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCImageTagInspectSaveTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
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
        auto verifyAnonymousVolumeMount = [](const auto& inspect) {
            VERIFY_ARE_EQUAL(inspect.Mounts.size(), 1u);
            VERIFY_ARE_EQUAL(inspect.Mounts[0].Type, "volume");
            VERIFY_IS_FALSE(inspect.Mounts[0].Name.empty());
            VERIFY_IS_TRUE(inspect.Mounts[0].Source.empty());
            VERIFY_ARE_EQUAL(inspect.Mounts[0].Destination, "/volume");
            VERIFY_IS_TRUE(inspect.Mounts[0].ReadWrite);
        };

        // Session-restart scenario: an anonymous volume-backed container survives a session reset.
        {
            WSLCContainerLauncher launcher("wslc-test-build:latest", "wslc-test-anonymous-volume", {"test", "-d", "/volume"});
            auto container = launcher.Launch(*m_defaultSession);
            container.SetDeleteOnClose(false);
            verifyAnonymousVolumeMount(container.Inspect());

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

            auto recoveredContainer = OpenContainer(m_defaultSession.get(), containerId);
            recoveredContainer.SetDeleteOnClose(false);
            verifyAnonymousVolumeMount(recoveredContainer.Inspect());
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

    WSLC_TEST_METHOD(ContainerInspectDockerfileVolumes)
    {
        const auto contextDir = std::filesystem::current_path() / "container-inspect-volume-build-context";
        constexpr auto imageName = "wslc-test-container-inspect-volume:latest";
        std::filesystem::create_directories(contextDir);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            std::error_code ec;
            std::filesystem::remove_all(contextDir, ec);
            LOG_IF_FAILED(DeleteImageNoThrow(imageName, WSLCDeleteImageFlagsForce).first);
        });

        {
            std::ofstream dockerfile(contextDir / "Dockerfile");
            dockerfile << "FROM debian:latest\n";
            dockerfile << "VOLUME [\"/volume-a\", \"/volume-b\"]\n";
        }

        VERIFY_SUCCEEDED(BuildImageFromContext(contextDir, imageName));

        WSLCContainerLauncher launcher(imageName, "wslc-test-container-inspect-volume");
        auto container = launcher.Create(*m_defaultSession);
        const auto inspect = container.Inspect();

        VERIFY_ARE_EQUAL(inspect.Mounts.size(), 2u);
        for (const auto* destination : {"/volume-a", "/volume-b"})
        {
            const auto mount =
                std::ranges::find_if(inspect.Mounts, [&](const auto& entry) { return entry.Destination == destination; });
            VERIFY_IS_TRUE(mount != inspect.Mounts.end());
            VERIFY_ARE_EQUAL(mount->Type, "volume");
            VERIFY_IS_FALSE(mount->Name.empty());
            VERIFY_IS_TRUE(mount->Source.empty());
            VERIFY_IS_TRUE(mount->ReadWrite);
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
};
