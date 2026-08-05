/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerRemoveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerRemoveTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerRemoveTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        BuildAnonymousVolumeImage();
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureVolumeDoesNotExist(TestVolumeName);
        RunWslc(std::format(L"image delete --force {}", AnonymousVolumeImage));
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureVolumeDoesNotExist(TestVolumeName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_HelpCommand)
    {
        auto result = RunWslc(L"container remove --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");

        // Delete the container
        result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer listed
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_ById_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"created");

        result = RunWslc(std::format(L"container remove {}", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId);
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Force_RunningContainer)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Run a container so it is in running state
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        // Removing without force should fail
        result = RunWslc(std::format(L"container remove {}", containerId));

        // TODO Add .Stderr after this issue is resolved:
        // https://github.com/microsoft/WSL/issues/14510
        result.Verify({.ExitCode = 1});

        // Container should still exist and be running
        VerifyContainerIsListed(containerId, L"running");

        // Removing with force should succeed
        result = RunWslc(std::format(L"container remove --force {}", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId);
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Multiple_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId1 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId1.empty());

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId2 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId2.empty());

        VerifyContainerIsListed(containerId1, L"created");
        VerifyContainerIsListed(containerId2, L"created");

        result = RunWslc(std::format(L"container remove {} {}", containerId1, containerId2));
        result.Verify({.Stdout = std::format(L"{}\r\n{}\r\n", containerId1, containerId2), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId1);
        VerifyContainerIsNotListed(containerId2);
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Volumes_RemovesAnonymousVolume)
    {
        const auto volumesBefore = ListVolumeNames();

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, AnonymousVolumeImage));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto anonymousVolume = GetNewVolumeName(volumesBefore);
        VerifyVolumeIsListed(anonymousVolume);

        result = RunWslc(std::format(L"container remove --volumes {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(WslcContainerName);
        VerifyVolumeIsNotListed(anonymousVolume);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_WithoutVolumes_KeepsAnonymousVolume)
    {
        const auto volumesBefore = ListVolumeNames();

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, AnonymousVolumeImage));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto anonymousVolume = GetNewVolumeName(volumesBefore);
        auto cleanup = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(anonymousVolume); });
        VerifyVolumeIsListed(anonymousVolume);

        result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(WslcContainerName);
        VerifyVolumeIsListed(anonymousVolume);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Volumes_KeepsNamedVolume)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container create --name {} -v {}:/data {}", WslcContainerName, TestVolumeName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container remove --volumes {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(WslcContainerName);

        // Only anonymous volumes are removed, named volumes must survive.
        VerifyVolumeIsListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Remove_Volumes_Force_RunningContainer)
    {
        const auto volumesBefore = ListVolumeNames();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, AnonymousVolumeImage));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        const auto anonymousVolume = GetNewVolumeName(volumesBefore);
        VerifyVolumeIsListed(anonymousVolume);
        VerifyContainerIsListed(containerId, L"running");

        // -fv exercises the combined short form of --force and --volumes.
        result = RunWslc(std::format(L"container rm -fv {}", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId);
        VerifyVolumeIsNotListed(anonymousVolume);
    }

private:
    // Builds a throwaway image whose VOLUME directive gives every container created from it an
    // anonymous volume. The CLI has no way to request one directly: -v always requires a source.
    static void BuildAnonymousVolumeImage()
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-container-remove-volume-image";
        auto cleanup = SetupTestDirectory(testRoot);

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath, std::format("FROM {}\nVOLUME /data\n", wsl::shared::string::WideToMultiByte(DebianTestImage().NameAndTag())));

        auto result =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" -t {}", testRoot.wstring(), dockerfilePath.wstring(), AnonymousVolumeImage));
        result.Verify({.ExitCode = 0});
    }

    // Returns the names of every volume currently in the session.
    static std::vector<std::wstring> ListVolumeNames()
    {
        auto result = RunWslc(L"volume list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        return result.GetStdoutLines();
    }

    // Returns the single volume name that appeared since the supplied snapshot was taken.
    static std::wstring GetNewVolumeName(const std::vector<std::wstring>& before)
    {
        std::vector<std::wstring> added;
        for (const auto& name : ListVolumeNames())
        {
            if (std::find(before.begin(), before.end(), name) == before.end())
            {
                added.push_back(name);
            }
        }

        VERIFY_ARE_EQUAL(static_cast<size_t>(1), added.size());
        return added.front();
    }

    static constexpr auto AnonymousVolumeImage = L"wslc-e2e-container-remove-anon:latest";

    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
    const std::wstring TestVolumeName = L"wslc-e2e-container-remove-volume";
    const TestImage& DebianImage = DebianTestImage();
};
} // namespace WSLCE2ETests