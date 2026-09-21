/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerVolumeTests.cpp

Abstract:

    This file contains test cases for WSLC container volume mounts.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCContainerVolumeTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCContainerVolumeTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
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
        auto hostFolder = wsl::windows::common::filesystem::GetCanonicalPath(std::filesystem::current_path() / "test-volume");
        auto symlinkFolder =
            wsl::windows::common::filesystem::GetCanonicalPath(std::filesystem::current_path() / "test-volume-symlink");
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
                "Failed to create volume '*test-volume\\subfolder': Access is denied.");
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
};
