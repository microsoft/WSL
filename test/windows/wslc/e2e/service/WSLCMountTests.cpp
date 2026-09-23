/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCMountTests.cpp

Abstract:

    This file contains test cases for the WSLC session mount API.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCMountTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCMountTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
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

        // Validate writable mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false, TRUE));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(false));

            // Validate that mount can't be stacked on each other
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "/win-path", false, TRUE), HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

            // Validate that folder is writable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt && sync"}, 0);
            VERIFY_ARE_EQUAL(ReadFileContent(testFolder / "file.txt"), L"content");

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path", TRUE));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate read-only mount.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true, TRUE));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Validate that folder is not writable from linux
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path", TRUE));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate that a read-only share cannot be made writable via mount -o remount,rw.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true, TRUE));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Attempt an in-place remount to read-write from the guest.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "mount -o remount,rw /win-path"}, 0);

            // Verify the folder is still not writable.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path/file.txt"}, 1);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path", TRUE));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate that the device host enforces read-only even if the guest tries to bypass mount options.
        if (enableVirtioFs)
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true, TRUE));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            // Remount a bind of the share as read-write to ensure the device host still enforces read-only access.
            ExpectCommandResult(
                session.get(),
                {"/bin/sh",
                 "-c",
                 "mkdir -p /win-path-rw && "
                 "mount --bind /win-path /win-path-rw && "
                 "mount -o remount,bind,rw /win-path-rw && "
                 "findmnt -n -o VFS-OPTIONS /win-path-rw | grep -qE '(^|,)rw(,|$)'"},
                0);

            // Verify the folder is still not writable through the read-write bind.
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "echo -n content > /win-path-rw/file.txt"}, 1);
            ExpectCommandResult(session.get(), {"/bin/sh", "-c", "umount /win-path-rw && rmdir /win-path-rw"}, 0);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path", TRUE));
            ExpectMount(session.get(), "/win-path", {});
        }

        // Validate various error paths
        {
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"relative-path", "/win-path", true, TRUE), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(L"C:\\does-not-exist", "/win-path", true, TRUE), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "relative-mountpoint", true, TRUE), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->MountWindowsFolder(testFolder.c_str(), "", true, TRUE), E_INVALIDARG);
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/not-mounted", TRUE), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            VERIFY_ARE_EQUAL(session->UnmountWindowsFolder("/proc", TRUE), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

            // Validate that folders that are manually unmounted from the guest are handled properly
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path", true, TRUE));
            ExpectMount(session.get(), "/win-path", expectedMountOptions(true));

            ExpectCommandResult(session.get(), {"/usr/bin/umount", "/win-path"}, 0);
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path", TRUE));
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

    // Validates that virtiofs mounts preserve file ownership for non-root users (regression test for #40719).
    WSLC_TEST_METHOD(WindowsMountsVirtioFsFileOwnership)
    {
        auto settings = GetDefaultSessionSettings(L"virtiofs-ownership-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);

        auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        auto testFolder = std::filesystem::current_path() / "test-folder-virtiofs-ownership";
        std::filesystem::create_directories(testFolder);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        static constexpr auto mountPoint = "/virtiofs-ownership-test";

        VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), mountPoint, false, TRUE));

        // Create a file and chown to uid 1000:100, then verify ownership is preserved.
        // Without the 'metadata' option on the virtiofs share, chown appears to succeed but
        // subsequent stat reports uid=0/gid=0 because ownership is not persisted.
        auto result = ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "touch /virtiofs-ownership-test/owned.txt && chown 1000:100 /virtiofs-ownership-test/owned.txt"
             " && stat -c '%u %g' /virtiofs-ownership-test/owned.txt"},
            0);

        VERIFY_ARE_EQUAL(result.Output[1], std::string("1000 100\n"));

        // Verify that a file created by a non-root user retains the creator's ownership.
        result = ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "rm -f /virtiofs-ownership-test/nonroot.txt"
             " && su -s /bin/sh nobody -c 'touch /virtiofs-ownership-test/nonroot.txt'"
             " && stat -c '%u' /virtiofs-ownership-test/nonroot.txt"},
            0);

        VERIFY_ARE_EQUAL(result.Output[1], std::string("65534\n"));

        VERIFY_SUCCEEDED(session->UnmountWindowsFolder(mountPoint, TRUE));
    }

    // Validates that each mount owns an independent child on the shared aggregate device.
    WSLC_TEST_METHOD(WindowsMountsVirtioFsIndependentShares)
    {
        auto settings = GetDefaultSessionSettings(L"virtiofs-independent-shares-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);

        auto createNewSession = !WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        auto testFolder = std::filesystem::current_path() / "test-folder-independent-shares";
        std::filesystem::create_directories(testFolder);
        std::ofstream(testFolder / "marker.txt") << "content";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testFolder); });

        auto getMountField = [&](const char* mountPoint, const char* field) -> std::string {
            auto cmd = std::format("findmnt -n -o {} {}", field, mountPoint);
            auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", cmd}, 0);
            return result.Output[1];
        };

        // Concurrent mounts of the same host path use distinct children on the same aggregate device.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path-1", false, TRUE));
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path-2", false, TRUE));

            auto firstDevice = getMountField("/win-path-1", "MAJ:MIN");
            auto secondDevice = getMountField("/win-path-2", "MAJ:MIN");
            auto firstRoot = getMountField("/win-path-1", "FSROOT");
            auto secondRoot = getMountField("/win-path-2", "FSROOT");
            VERIFY_ARE_EQUAL(firstDevice, secondDevice);
            VERIFY_ARE_NOT_EQUAL(firstRoot, secondRoot);
            VERIFY_IS_TRUE(firstRoot.starts_with('/'));
            VERIFY_IS_TRUE(firstRoot.ends_with('\n'));
            VERIFY_IS_TRUE(secondRoot.starts_with('/'));
            VERIFY_IS_TRUE(secondRoot.ends_with('\n'));
            firstRoot.pop_back();
            secondRoot.pop_back();

            const auto firstChild = std::format("/run/wsl/virtiofs-mounts/{}{}", LX_INIT_DRVFS_VIRTIO_TAG, firstRoot);
            const auto secondChild = std::format("/run/wsl/virtiofs-mounts/{}{}", LX_INIT_DRVFS_VIRTIO_TAG, secondRoot);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path-1", TRUE));
            ExpectCommandResult(session.get(), {"/bin/cat", "/win-path-2/marker.txt"}, 0);
            ExpectCommandResult(session.get(), {"/usr/bin/test", "!", "-e", firstChild}, 0);
            ExpectCommandResult(session.get(), {"/usr/bin/test", "-e", secondChild}, 0);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path-2", TRUE));
            ExpectCommandResult(session.get(), {"/usr/bin/test", "!", "-e", secondChild}, 0);
        }

        // Verify that read-write and read-only shares use different children on the same aggregate device.
        {
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path-rw", false, TRUE));
            VERIFY_SUCCEEDED(session->MountWindowsFolder(testFolder.c_str(), "/win-path-ro", true, TRUE));

            auto rwDevice = getMountField("/win-path-rw", "MAJ:MIN");
            auto roDevice = getMountField("/win-path-ro", "MAJ:MIN");
            auto rwRoot = getMountField("/win-path-rw", "FSROOT");
            auto roRoot = getMountField("/win-path-ro", "FSROOT");

            VERIFY_ARE_EQUAL(rwDevice, roDevice);
            VERIFY_ARE_NOT_EQUAL(rwRoot, roRoot);

            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path-rw", TRUE));
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/win-path-ro", TRUE));
        }
    }

    WSLC_TEST_METHOD(WindowsMountsVirtioFsRemoveChild)
    {
        auto settings = GetDefaultSessionSettings(L"virtiofs-remove-child-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);
        auto session = CreateSession(settings);

        const auto testRoot = std::filesystem::current_path() / "test-folder-remove-child";
        const auto firstFolder = testRoot / "first";
        const auto secondFolder = testRoot / "second";
        std::filesystem::create_directories(firstFolder);
        std::filesystem::create_directories(secondFolder);
        std::ofstream(firstFolder / "marker.txt") << "first";
        std::ofstream(secondFolder / "marker.txt") << "second";
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testRoot); });

        auto getMountRoot = [&](const char* mountPoint) {
            const auto command = std::format("findmnt -n -o FSROOT {}", mountPoint);
            auto root = ExpectCommandResult(session.get(), {"/bin/sh", "-c", command}, 0).Output.at(1);
            VERIFY_IS_TRUE(root.starts_with('/'));
            VERIFY_IS_TRUE(root.ends_with('\n'));
            root.pop_back();
            return root;
        };

        VERIFY_SUCCEEDED(session->MountWindowsFolder(firstFolder.c_str(), "/remove-child-first", false, TRUE));
        VERIFY_SUCCEEDED(session->MountWindowsFolder(secondFolder.c_str(), "/remove-child-second", false, TRUE));

        const auto firstRoot = getMountRoot("/remove-child-first");
        const auto secondRoot = getMountRoot("/remove-child-second");
        VERIFY_ARE_NOT_EQUAL(firstRoot, secondRoot);

        const auto aggregateRoot = std::format("/run/wsl/virtiofs-mounts/{}", LX_INIT_DRVFS_VIRTIO_TAG);
        const auto firstChild = aggregateRoot + firstRoot;
        const auto secondChild = aggregateRoot + secondRoot;
        ExpectCommandResult(session.get(), {"/usr/bin/test", "-e", firstChild}, 0);
        ExpectCommandResult(session.get(), {"/usr/bin/test", "-e", secondChild}, 0);

        VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/remove-child-first", TRUE));
        ExpectCommandResult(session.get(), {"/usr/bin/test", "!", "-e", firstChild}, 0);
        ExpectCommandResult(session.get(), {"/bin/cat", "/remove-child-second/marker.txt"}, 0);
        ExpectCommandResult(session.get(), {"/usr/bin/test", "-e", secondChild}, 0);

        VERIFY_SUCCEEDED(session->UnmountWindowsFolder("/remove-child-second", TRUE));
        ExpectCommandResult(session.get(), {"/usr/bin/test", "!", "-e", secondChild}, 0);
    }

    // Validate that enough VirtioFs shares can be mounted to exceed the old per-device aperture limit.
    WSLC_TEST_METHOD(VirtiofsMountManyVolumes)
    {
        constexpr size_t c_shareCount = 32;

        auto settings = GetDefaultSessionSettings(L"virtiofs-many-shares-test");
        WI_SetFlag(settings.FeatureFlags, WslcFeatureFlagsVirtioFs);

        auto session = CreateSession(settings);

        auto testRoot = std::filesystem::current_path() / "test-folder-many-shares";
        std::filesystem::create_directories(testRoot);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove_all(testRoot); });
        std::vector<std::string> mountPoints;

        for (size_t index = 0; index < c_shareCount; ++index)
        {
            const auto folder = testRoot / std::to_string(index);
            std::filesystem::create_directories(folder);
            std::ofstream(folder / "marker.txt") << index;

            const auto mountPoint = std::format("/vfs-many-{}", index);
            VERIFY_SUCCEEDED(session->MountWindowsFolder(folder.c_str(), mountPoint.c_str(), false, TRUE));
            mountPoints.emplace_back(mountPoint);

            const auto command = std::format("cat {}/marker.txt", mountPoint);
            const auto result = ExpectCommandResult(session.get(), {"/bin/sh", "-c", command}, 0);
            VERIFY_ARE_EQUAL(std::to_string(index), result.Output.at(1));
        }

        for (const auto& mountPoint : mountPoints)
        {
            VERIFY_SUCCEEDED(session->UnmountWindowsFolder(mountPoint.c_str(), TRUE));
        }
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

            // Validate that the mount points are not writable.
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
            VERIFY_ARE_EQUAL(E_INVALIDARG, m_defaultSession->CreateRootNamespaceProcess("/bin/true", &options, 0, 0, FALSE, &process, &err));
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
};
