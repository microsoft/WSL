/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    MountTests.cpp

Abstract:

    This file contains test cases for the disk mounting logic.

--*/

#include "precomp.h"
#include "Common.h"

#define TEST_MOUNT_DISK L"TestDisk.vhd"
#define TEST_MOUNT_VHD L"TestVhd.vhd"
#define TEST_UNMOUNT_VHD_DNE L"TestVhdNotHere.vhd"
#define TEST_MOUNT_NAME L"testmount"

#define SKIP_UNSUPPORTED_ARM64_MOUNT_TEST() \
    if constexpr (wsl::shared::Arm64) \
    { \
        WSL_TEST_VERSION_REQUIRED(27653); \
    }

namespace MountTests {

// Disks sometimes take a bit of time to become available when attached back to the host.
constexpr auto c_diskOpenTimeoutMs = 120000;

class SetAutoMountPolicy
{
public:
    SetAutoMountPolicy() = delete;
    SetAutoMountPolicy(const SetAutoMountPolicy&) = delete;
    SetAutoMountPolicy& operator=(const SetAutoMountPolicy&) = delete;

    SetAutoMountPolicy(SetAutoMountPolicy&& Other) = default;
    SetAutoMountPolicy& operator=(SetAutoMountPolicy&&) = default;

    SetAutoMountPolicy(bool Enable) : PreviousState(GetAutoMountState())
    {
        if (Enable != PreviousState)
        {
            SetAutoMountState(Enable);
        }
        else
        {
            PreviousState.reset();
        }
    }

    ~SetAutoMountPolicy()
    {
        if (PreviousState.has_value())
        {
            SetAutoMountState(PreviousState.value());
        }
    }

private:
    static bool GetAutoMountStateFromOutput(const std::wstring& Output)
    {
        if (Output.find(L"Automatic mounting of new volumes enabled") != std::wstring::npos)
        {
            return true;
        }
        else if (Output.find(L"Automatic mounting of new volumes disabled") != std::wstring::npos)
        {
            return false;
        }

        LogError("Unexpected diskpart output: '%s'", Output.c_str());
        VERIFY_FAIL(L"Failed to parse diskpart's output");
        return false;
    }

    static bool GetAutoMountState()
    {
        std::wstring cmd = L"diskpart.exe";
        return GetAutoMountStateFromOutput(LxsstuLaunchCommandAndCaptureOutput(cmd.data(), "automount\r\n").first);
    }

    static void SetAutoMountState(bool Enabled)
    {
        LogInfo("Setting automount policy to %i", Enabled);

        std::wstring cmd = L"diskpart.exe";
        const auto input = std::string("automount ") + (Enabled ? "enable\r\n" : "disable\r\n");
        auto [output, _] = LxsstuLaunchCommandAndCaptureOutput(cmd.data(), input.c_str());

        VERIFY_ARE_EQUAL(Enabled, GetAutoMountStateFromOutput(output));
    }

    std::optional<bool> PreviousState;
};

class MountTests
{
    std::wstring DiskDevice;
    std::wstring VhdDevice;
    wil::unique_tokeninfo_ptr<TOKEN_USER> User = wil::get_token_information<TOKEN_USER>();
    std::unique_ptr<wsl::windows::common::security::privilege_context> PrivilegeState;
    DWORD DiskNumber = 0;
    SetAutoMountPolicy AutoMountPolicy{false};

    struct ExpectedMountState
    {
        size_t PartitionIndex;
        std::optional<std::wstring> Type;
        std::optional<std::wstring> Options;
    };

    struct ExpectedDiskState
    {
        std::wstring Path;
        std::vector<ExpectedMountState> Mounts;
    };

    WSL_TEST_CLASS(MountTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(false), TRUE);

        if (!LxsstuVmMode())
        {
            return true;
        }

        // Needed to open processes under te
        PrivilegeState = wsl::windows::common::security::AcquirePrivilege(SE_DEBUG_NAME);

        // Create a 20MB vhd for testing mounting passthrough disks
        DeleteFileW(TEST_MOUNT_DISK);

        try
        {
            LxsstuLaunchPowershellAndCaptureOutput(L"New-Vhd -Path " TEST_MOUNT_DISK " -SizeBytes 20MB");
        }
        CATCH_LOG()

        // Mount it in Windows
        auto [output, _] = LxsstuLaunchPowershellAndCaptureOutput(L"(Mount-VHD " TEST_MOUNT_DISK " -PassThru | Get-Disk).Number");

        Trim(output);
        DiskNumber = std::stoul(output);

        // Construct the disk path
        DiskDevice = L"\\\\.\\PhysicalDrive" + output;
        LogInfo("Mounted the passthrough test vhd as %ls", DiskDevice.c_str());

        // Create a 20MB vhd for testing mount --vhd
        DeleteFileW(TEST_MOUNT_VHD);

        LxsstuLaunchPowershellAndCaptureOutput(L"New-Vhd -Path " TEST_MOUNT_VHD " -SizeBytes 20MB");

        VhdDevice = wsl::windows::common::filesystem::GetFullPath(TEST_MOUNT_VHD);
        LogInfo("Create mount --vhd test vhd as %ls", VhdDevice.c_str());

        return true;
    }

    // Uninitialize the tests.
    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (LxsstuVmMode())
        {
            PrivilegeState.reset();

            LxsstuLaunchWsl(L"--unmount");
            WaitForDiskReady();

            try
            {
                LxsstuLaunchPowershellAndCaptureOutput(L"Dismount-Vhd -Path " TEST_MOUNT_DISK);
            }
            CATCH_LOG()

            DeleteFileW(TEST_MOUNT_DISK);
            DeleteFileW(TEST_MOUNT_VHD);
        }

        VERIFY_NO_THROW(LxsstuUninitialize(false));
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        if (!LxsstuVmMode())
        {
            return true;
        }

        LxssLogKernelOutput();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount"), (DWORD)0);
        WaitForDiskReady();

        return true;
    }

    // Attach a vhd, but don't mount it
    TEST_METHOD(TestBareMountVhd)
    {
        TestBareMountImpl(true);
    }

    // Mount one partition using --vhd and validate that options are correctly applied
    TEST_METHOD(TestMountOnePartitionVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountOnePartitionImpl(true);
    }

    // Mount two partitions using --vhd on the same disk
    TEST_METHOD(TestMountTwoPartitionsVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountTwoPartitionsImpl(true);
    }

    // Run a bare mount using --vhd and then mount a partition
    TEST_METHOD(TestAttachThenMountVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestAttachThenMountImpl(true);
    }

    // Mount the disk directly
    TEST_METHOD(TestMountWholeDiskVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountWholeDiskImpl(true);
    }

    // Test that mount state is deleted on shutdown (--vhd)
    TEST_METHOD(TestMountStateIsDeletedOnShutdownVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountStateIsDeletedOnShutdownImpl(true);
    }

    TEST_METHOD(TestFilesystemDetectionWholeDisk)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestFilesystemDetectionWholeDiskImpl(false);
    }

    TEST_METHOD(TestFilesystemDetectionWholeDiskVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestFilesystemDetectionWholeDiskImpl(true);
    }

    TEST_METHOD(TestMountTwoPartitionsWithDetection)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountTwoPartitionsWithDetectionImpl(false);
    }

    TEST_METHOD(TestMountTwoPartitionsWithDetectionVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountTwoPartitionsWithDetectionImpl(true);
    }

    TEST_METHOD(TestFilesystemDetectionFail)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestFilesystemDetectionFailImpl(false);
    }

    TEST_METHOD(TestFilesystemDetectionFailVhd)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestFilesystemDetectionFailImpl(true);
    }

    // Test specifying a mount name for a vhd
    TEST_METHOD(SpecifyMountName)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        const auto mountCommand = L"--mount " + VhdDevice + L" --vhd --name " + TEST_MOUNT_NAME;

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, true);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        const std::wstring diskName(TEST_MOUNT_NAME);
        auto mountTarget = L"/mnt/wsl/" + diskName;

        ValidateMountPoint(disk + L"1", mountTarget);

        ValidateDiskState({VhdDevice, {{1, {}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + VhdDevice), (DWORD)0);
        WaitForDiskReady();

        // Validate that the mount folder was deleted
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e " + mountTarget), (DWORD)1);

        // Mount the same partition, but with a specific mount option
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1 --options \"data=ordered\""), (DWORD)0);

        // Validate that the mount option was properly passed
        disk = GetBlockDeviceInWsl();
        ValidateMountPoint(disk + L"1", mountTarget, L"data=ordered");
        ValidateDiskState({VhdDevice, {{1, {}, L"data=ordered"}}}, keepAlive);

        // Let the VM timeout
        WaitForVmTimeout(keepAlive);

        // Validate that the disk is re-mounted in the same place
        disk = GetBlockDeviceInWsl();
        ValidateMountPoint(disk + L"1", mountTarget);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + VhdDevice), (DWORD)0);
        WaitForDiskReady();
    }

    // Test ensuring that name collision detection works in --mount --name
    TEST_METHOD(SpecifyMountNameCollision)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        const auto mountCommand = L"--mount " + VhdDevice + L" --vhd --name " + TEST_MOUNT_NAME;

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition and one fat partitions
        FormatDisk({L"ext4", L"vfat"}, true);

        // Attempt to mount both partitions with the same mount name; partition 2 should fail
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);
        VERIFY_ARE_NOT_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 2 --type vfat"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount first mount did succeed
        const std::wstring diskName(TEST_MOUNT_NAME);
        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + diskName, {}, L"ext4");

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + VhdDevice), (DWORD)0);
        WaitForDiskReady();
    }

    // Test that multiple partitions can be mounted with --name
    TEST_METHOD(SpecifyMountNameTwoPartitions)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        const auto mountCommandOne = L"--mount " + VhdDevice + L" --vhd --name " + TEST_MOUNT_NAME + L"p1";
        const auto mountCommandTwo = L"--mount " + VhdDevice + L" --vhd --name " + TEST_MOUNT_NAME + L"p2";

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition and one fat partitions
        FormatDisk({L"ext4", L"vfat"}, true);

        // Attempt to mount both partitions with the same mount name; partition 2 should fail
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommandOne + L" --partition 1"), (DWORD)0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommandTwo + L" --partition 2 --type vfat"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount first mount did succeed
        const std::wstring diskName(TEST_MOUNT_NAME);
        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + diskName + L"p1", {}, L"ext4");
        ValidateMountPoint(disk + L"2", L"/mnt/wsl/" + diskName + L"p2", {}, L"vfat");
        ValidateDiskState({VhdDevice, {{1, {}, {}}, {2, {L"vfat"}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + VhdDevice), (DWORD)0);
        WaitForDiskReady();
    }

    // Test relative mount/unmounting of a --vhd
    TEST_METHOD(RelativePathUnmount)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " TEST_MOUNT_VHD L" --vhd --bare"), (DWORD)0);

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " TEST_MOUNT_VHD), (DWORD)0);
    }

    // Test relative mount/unmounting of a --vhd that does not exist
    TEST_METHOD(RelativePathUnmountNoFileExists)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " TEST_MOUNT_VHD L" --vhd --bare"), (DWORD)0);

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Try unmounting a VHD not created and verify that it was not successful
        VERIFY_ARE_NOT_EQUAL(LxsstuLaunchWsl(L"--unmount " TEST_UNMOUNT_VHD_DNE), (DWORD)0);
    }

    TEST_METHOD(AbsolutePathVhdUnmount)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " TEST_MOUNT_VHD L" --vhd --bare"), (DWORD)0);

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        const auto absolutePath = std::filesystem::absolute(TEST_MOUNT_VHD);

        // Validate that the vhd path doesn't start with '\\?'
        VERIFY_IS_FALSE(absolutePath.wstring().starts_with(L"\\"));

        // Validate the unmounting by absolute path is successful
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + absolutePath.wstring()), (DWORD)0);
    }

    // Attach a disk, but don't mount it
    TEST_METHOD(TestBareMount)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestBareMountImpl(false);
    }

    // Validate that attached disks that were offline when attached
    // are still offline when detached
    TEST_METHOD(TestOfflineDiskStaysOffline)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        auto diskHandle = wsl::windows::common::disk::OpenDevice(DiskDevice.c_str(), GENERIC_ALL, c_diskOpenTimeoutMs);
        wsl::windows::common::disk::SetOnline(diskHandle.get(), false);
        diskHandle.reset();

        ValidateOffline(true);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --bare"), (DWORD)0);

        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        ValidateDiskState({DiskDevice, {}}, keepAlive);

        disk = GetBlockDeviceInWsl();
        VERIFY_IS_FALSE(GetBlockDeviceMount(disk).has_value());

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);

        ValidateOffline(true);
        diskHandle = wsl::windows::common::disk::OpenDevice(DiskDevice.c_str(), GENERIC_ALL, c_diskOpenTimeoutMs);
        wsl::windows::common::disk::SetOnline(diskHandle.get(), true);
        diskHandle.reset();

        ValidateOffline(false);
    }

    // Mount one partition and validate that options are correctly applied
    TEST_METHOD(TestMountOnePartition)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountOnePartitionImpl(false);
    }

    // Mount two partitions on the same disk
    TEST_METHOD(TestMountTwoPartitions)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountTwoPartitionsImpl(false);
    }

    // Mount a fat partition
    TEST_METHOD(TestMountFatPartition)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ntfs partition
        FormatDisk({L"vfat"}, false);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1" + L" --type vfat"), (DWORD)0);

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);
        auto mountTarget = L"/mnt/wsl/" + trimmedDiskName + L"p1";
        ValidateMountPoint(disk + L"1", mountTarget, {}, L"vfat");
        ValidateDiskState({DiskDevice, {{1, {L"vfat"}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
        WaitForDiskReady();
        ValidateOffline(false);
    }

    // Mount the disk directly
    TEST_METHOD(TestMountWholeDisk)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountWholeDiskImpl(false);
    }

    TEST_METHOD(TestMountStateIsDeletedOnShutdown)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestMountStateIsDeletedOnShutdownImpl(false);
    }

    // Validate that a failure to mount a disk isn't fatal
    TEST_METHOD(TestMountFailuresArentFatal)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, false);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1 --type ext4"), (DWORD)0);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        ValidateDiskState({DiskDevice, {{1, {L"ext4"}, {}}}}, keepAlive);

        // Check that the disk is still mounted properly (ValidateDiskState restarts the VM)
        disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);
        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, L"ext4");

        // Wait for vm timeout
        WaitForVmTimeout(keepAlive);

        // Voluntarily set a wrong filesystem in the saved state
        auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(User->User.Sid);
        auto subKeys = wsl::windows::common::registry::EnumKeys(key.get(), KEY_ALL_ACCESS);
        VERIFY_ARE_EQUAL(subKeys.size(), 1);

        wsl::windows::common::registry::WriteString(subKeys.begin()->second.get(), L"1", L"Type", L"badfs");
        keepAlive.Set();

        // The disk should be present
        disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // But not mounted
        ValidateMountPoint(disk + L"1", {});

        // Now put a bad disk path, so that the disk fails to attach
        WaitForVmTimeout(keepAlive);
        key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(User->User.Sid);
        subKeys = wsl::windows::common::registry::EnumKeys(key.get(), KEY_ALL_ACCESS);
        VERIFY_ARE_EQUAL(subKeys.size(), 1);
        wsl::windows::common::registry::WriteString(subKeys.begin()->second.get(), nullptr, L"Disk", L"BadDisk");
        keepAlive.Reset();

        // Restart the service
        RestartWslService();

        // Run a dummy command to trigger a VM start
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo foo"), (DWORD)0);

        // The disk should still be online, because it failed to attach
        ValidateOffline(false);
    }

    // wsl --unmount should succeed even when no disk is mounted
    TEST_METHOD(UnmountWithoutAnyDisk)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount"), (DWORD)0);
    }

    // Mount two partitions on the same disk and validate that the mount is restored
    TEST_METHOD(TestMountTwoPartitionsAfterTimeout)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition and one fat partitions
        FormatDisk({L"ext4", L"vfat"}, false);

        // Mount then both
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1"), (DWORD)0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 2 --type vfat"), (DWORD)0);

        ValidateDiskState({DiskDevice, {{1, {}, {}}, {2, {L"vfat"}, {}}}}, keepAlive);

        // Validate that our disk is still mounted
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, L"ext4");
        ValidateMountPoint(disk + L"2", L"/mnt/wsl/" + trimmedDiskName + L"p2", {}, L"vfat");

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
    }

    // Validate that non-admin can remount saved disks
    TEST_METHOD(TestMount1PartitionAndRemountAsNonAdmin)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        FormatDisk({L"ext4"}, false);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1"), (DWORD)0);

        ValidateDiskState({DiskDevice, {{1, {}, {}}}}, keepAlive);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Let the UVM timeout
        WaitForVmTimeout(keepAlive);

        // Restart wsl as a non-elevated user
        const auto nonElevatedToken = GetNonElevatedToken();

        // Launch wsl non-elevated
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo dummy", nullptr, nullptr, nullptr, nonElevatedToken.get()), (DWORD)0);
        keepAlive.Set();

        // Validate that our disk is still attached
        disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, L"ext4");

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
    }

    // Run a bare mount and then mount a partition
    TEST_METHOD(TestAttachThenMount)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();

        TestAttachThenMountImpl(false);
    }

    // Validate that unmounting works when the UVM is not running
    TEST_METHOD(TestMountOnePartitionAfterTimeout)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, false);

        // Mount it
        ValidateOffline(false);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));
        ValidateOffline(true);

        // Wait for vm timeout
        WaitForVmTimeout(keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);

        // The UVM shouldn't be running
        VERIFY_IS_FALSE(GetVmmempPid().has_value());

        // No state should be left in registry
        const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(User->User.Sid);
        VERIFY_ARE_EQUAL(wsl::windows::common::registry::EnumKeys(key.get(), KEY_READ).size(), 0);
    }

    // Validate that the proper mount error is returned if the filesystem type is wrong
    TEST_METHOD(TestMountPartitionWithWrongFs)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, false);

        // Mount it
        wsl::windows::common::SvcComm service;
        VERIFY_ARE_EQUAL(service.AttachDisk(DiskDevice.c_str(), LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH), S_OK);

        const auto result = service.MountDisk(DiskDevice.c_str(), LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH, 1, nullptr, L"vfat", nullptr);

        VERIFY_ARE_EQUAL(result.Result, -22); //-EINVAL
        VERIFY_ARE_EQUAL(result.Step, 3);     // LxMiniInitMountStepMount

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
    }

    // Validate that the proper mount error is returned if the partition can't be found
    TEST_METHOD(TestMountPartitionWithBadPartitionIndex)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 fat partition
        FormatDisk({L"vfat"}, false);

        // Try to mount a partition that doesn't exist
        wsl::windows::common::SvcComm service;
        VERIFY_ARE_EQUAL(service.AttachDisk(DiskDevice.c_str(), LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH), S_OK);

        const auto result = service.MountDisk(DiskDevice.c_str(), LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH, 2, nullptr, L"vfat", nullptr);

        VERIFY_ARE_EQUAL(result.Result, -2); // -ENOENT
        VERIFY_ARE_EQUAL(result.Step, 2);    // LxMiniInitMountStepFindPartition

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
    }

    // Validate that disk aren't detached if in use by other processes
    TEST_METHOD(TestDeviceCantBeMountedIfInUse)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        {
            // Format-Volume fails without automount enabled
            SetAutoMountPolicy AutoMountPolicy{true};

            // Reset the disk
            LxsstuLaunchPowershellAndCaptureOutput(L"Clear-Disk -confirm:$false -RemoveData -Number " + std::to_wstring(DiskNumber));

            LxsstuLaunchPowershellAndCaptureOutput(L"Initialize-Disk -confirm:$false -Number " + std::to_wstring(DiskNumber));

            // Create one fat partition
            LxsstuLaunchPowershellAndCaptureOutput(
                L"New-Partition -DiskNumber " + std::to_wstring(DiskNumber) +
                L" -UseMaximumSize \
                | Format-Volume -FileSystem FAT");
        }

        // Mount it in Windows
        auto [letter, _] = LxsstuLaunchPowershellAndCaptureOutput(
            L"Set-Partition  -DiskNumber " + std::to_wstring(DiskNumber) + L" -PartitionNumber 1" + L" -NewDriveLetter Y");

        // Open a file under that partition
        wil::unique_handle file(CreateFile(L"Y:\\foo.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));

        const char* fileContent = "LOW!";
        THROW_LAST_ERROR_IF(!WriteFile(file.get(), fileContent, static_cast<DWORD>(strlen(fileContent)), nullptr, nullptr));

        // Validate that the disk can't be mounted (TODO: Find a way to validate the failure reason)
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1 --type vfat"), (DWORD)-1);

        // Close the file and mount it
        file.reset();
        WaitForDiskReady();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1 --type vfat"), (DWORD)0);

        // Validate that the file content is correct
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", {L"/mnt/wsl/" + trimmedDiskName + L"p1"}, {}, L"vfat");
        auto [output, __] = LxsstuLaunchWslAndCaptureOutput(L"cat /mnt/wsl/" + trimmedDiskName + L"p1/foo.txt");

        VERIFY_ARE_EQUAL(output, wsl::shared::string::MultiByteToWide(fileContent));
    }

    TEST_METHOD(TestMountWithFlagOption)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, false);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1 --options sync"), (DWORD)0);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(DiskDevice);
        Trim(trimmedDiskName);
        auto mountTarget = L"/mnt/wsl/" + trimmedDiskName + L"p1";

        ValidateMountPoint(disk + L"1", mountTarget, L"sync");
        ValidateDiskState({DiskDevice, {{1, {}, L"sync"}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
        WaitForDiskReady();

        // Mount the same partition, but with both a flag and a non-flag option
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + DiskDevice + L" --partition 1 --options data=ordered,sync"), (DWORD)0);

        // Validate that the mount option was properly passed
        disk = GetBlockDeviceInWsl();

        ValidateMountPoint(disk + L"1", mountTarget, L"ync,relatime,data=ordered");

        // Note: relatime is set by default
        ValidateDiskState({DiskDevice, {{1, {}, L"data=ordered,sync"}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + DiskDevice), (DWORD)0);
        WaitForDiskReady();
    }

    TEST_METHOD(TestAttachFailsWithoutWsl2Distro)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL1_TEST_ONLY();

        // Attempt to mount a disk with only a WSL1 distro
        wsl::windows::common::SvcComm service;
        VERIFY_ARE_EQUAL(service.AttachDisk(L"Dummy", LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH), WSL_E_WSL2_NEEDED);
    }

    TEST_METHOD(VhdWithSpaces)
    {
        SKIP_UNSUPPORTED_ARM64_MOUNT_TEST();
        WSL2_TEST_ONLY();

        LxsstuLaunchPowershellAndCaptureOutput(L"New-Vhd -Path 'vhd with spaces.vhdx' -SizeBytes 20MB");

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            WslShutdown();

            if (!DeleteFile(L"vhd with spaces.vhdx"))
            {
                LogInfo("Failed to delete vhd, %i", GetLastError());
            };
        });

        WslKeepAlive keepAlive;

        // Validate that relative path mounting and unmounting works
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount \"vhd with spaces.vhdx\" --bare --vhd"), (DWORD)0);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount \"vhd with spaces.vhdx\""), (DWORD)0);

        // Validate that absolute path mounting and unmounting works
        const std::wstring fullPath = wsl::windows::common::filesystem::GetFullPath(L"vhd with spaces.vhdx");
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount \"" + fullPath + L"\" --bare --vhd"), (DWORD)0);
        disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount \"" + fullPath + L"\""), (DWORD)0);
    }

    void WaitForDiskReady() const
    {
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (timeout > std::chrono::steady_clock::now())
        {
            try
            {
                auto disk = wsl::windows::common::disk::OpenDevice(DiskDevice.c_str(), GENERIC_READ, c_diskOpenTimeoutMs);
                wsl::windows::common::disk::ValidateDiskVolumesAreReady(disk.get());
                return;
            }
            catch (...)
            {
                auto error = std::system_category().message(wil::ResultFromCaughtException());
                LogInfo("Caught '%S' while waiting for disk", error.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        VERIFY_FAIL(L"Timeout waiting for disk");
    }

    void ValidateOffline(bool offline) const
    {
        const auto disk = wsl::windows::common::disk::OpenDevice(DiskDevice.c_str(), FILE_READ_ATTRIBUTES, c_diskOpenTimeoutMs);
        VERIFY_ARE_EQUAL(!offline, wsl::windows::common::disk::IsDiskOnline(disk.get()));
    }

    static std::wstring GetBlockDeviceInWsl()
    {
        // Wait for the disk to be attached
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        bool done = false;
        while (true)
        {
            for (wchar_t name = 'a'; name < 'z'; name++)
            {
                std::wstring cmd = L"-u root blockdev --getsize64 /dev/sd";
                cmd += name;

                std::wstring out;
                try
                {
                    out = LxsstuLaunchWslAndCaptureOutput(cmd.data()).first;
                }
                CATCH_LOG()

                Trim(out);

                // Disk size is 20MB, so 20 * 1024 * 1024 bytes
                if (out == L"20971520")
                {
                    return std::wstring(L"/dev/sd") + name;
                }
            }

            if (done)
            {
                break;
            }

            done = std::chrono::steady_clock::now() > timeout;
        }

        VERIFY_FAIL(L"Failed to find the block device in WSL");

        // Unreachable.
        return {};
    }

    static bool IsBlockDevicePresent(const std::wstring& Device)
    {
        const auto Cmd = L"test -e " + Device;
        return LxsstuLaunchWsl(Cmd.data()) == 0;
    }

    static std::optional<std::vector<std::wstring>> GetBlockDeviceMount(const std::wstring& device)
    {
        const std::wstring cmd(L"cat /proc/mounts");
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(cmd.data());

        LogInfo("/proc/mounts content: '%ls'", out.c_str());
        std::wistringstream output(out);
        std::wstring line;

        while (std::getline(output, line))
        {
            if (wcsstr(line.data(), device.data()) == line.data())
            {
                return LxssSplitString(line);
            }
        }

        return {};
    }

    void ValidateDiskState(const ExpectedDiskState& State, WslKeepAlive& KeepAlive)
    {
        WaitForVmTimeout(KeepAlive);
        const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(User->User.Sid);
        const auto subKeys = wsl::windows::common::registry::EnumKeys(key.get(), KEY_READ);

        VERIFY_ARE_EQUAL(subKeys.size(), 1);

        const auto& diskKey = subKeys.begin()->second;

        auto read = [](const auto& Key, LPCWSTR Name) -> std::optional<std::wstring> {
            try
            {
                return wsl::windows::common::registry::ReadString(Key.get(), nullptr, Name);
            }
            catch (...)
            {
                return {};
            }
        };

        VERIFY_ARE_EQUAL(read(diskKey, L"Disk").value(), State.Path);
        VERIFY_ARE_EQUAL(wsl::windows::common::registry::EnumKeys(diskKey.get(), KEY_READ).size(), State.Mounts.size());

        for (const auto& e : State.Mounts)
        {
            auto keyName = std::to_wstring(e.PartitionIndex);

            auto mountKey = wsl::windows::common::registry::OpenKey(diskKey.get(), keyName.c_str(), KEY_READ);

            VERIFY_ARE_EQUAL(read(mountKey, L"Options"), e.Options);
            VERIFY_ARE_EQUAL(read(mountKey, L"Type"), e.Type);
        }

        KeepAlive.Set();
    }

    void WaitForVmTimeout(WslKeepAlive& KeepAlive)
    {
        const auto pid = GetVmmempPid();
        VERIFY_IS_TRUE(pid.has_value());
        KeepAlive.Reset();
        const std::wstring cmd = std::wstring(L"-t ") + std::wstring(LXSS_DISTRO_NAME_TEST_L);

        // Terminate the distro to make the vm timeout faster
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(cmd.c_str()), (DWORD)0);

        const wil::unique_process_handle process(OpenProcess(SYNCHRONIZE, false, pid.value()));
        VERIFY_IS_NOT_NULL(process.get());

        VERIFY_ARE_EQUAL((DWORD)WAIT_OBJECT_0, WaitForSingleObject(process.get(), INFINITE));
    }

    static std::optional<DWORD> GetVmmempPid()
    {
        for (auto pid : wsl::windows::common::wslutil::ListRunningProcesses())
        {
            wil::unique_process_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid));
            if (!process)
            {
                continue;
            }

            std::wstring imageName(MAX_PATH, '\0');
            const DWORD length = GetProcessImageFileName(process.get(), imageName.data(), (DWORD)imageName.size() + 1);
            if (length == 0)
            {
                continue;
            }

            imageName.resize(length);
            if (imageName == L"vmmemWSL" || (!wsl::windows::common::helpers::IsWindows11OrAbove() && imageName == L"vmmem"))
            {
                return pid;
            }
        }

        return {}; // Unreachable
    }

    void FormatDisk(const std::vector<std::wstring>& Partitions, bool isVhdTest)
    {
        WaitForDiskReady();
        const auto deviceName = (isVhdTest) ? VhdDevice : DiskDevice;
        if (isVhdTest)
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + deviceName + L" --vhd --bare"), (DWORD)0);
        }
        else
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--mount " + deviceName + L" --bare"), (DWORD)0);
        }

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Create a partition table
        std::wstringstream Cmd;
        Cmd << "bash -c \"(";
        Cmd << L"echo -e o\n"; // Create a new partition table

        for (size_t i = 0; i < Partitions.size(); i++)
        {
            Cmd << L"echo -e n\n";                             // Add a new partition
            Cmd << L"echo -e p\n";                             // Primary partition
            Cmd << L"echo -e " << (i + 1) << L"\n";            // Partition number
            Cmd << L"echo -e\n";                               // First sector (Accept default)
            Cmd << L"echo " << 2049 + (i + 1) * 4096 << L"\n"; // Last sector
        }

        Cmd << L"echo -e w\n"; // Write changes
        Cmd << L") | fdisk " + disk + L"\"";

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(Cmd.str()), (DWORD)0);

        for (size_t i = 1; i <= Partitions.size(); i++)
        {
            auto partition = disk + std::to_wstring(i);

            // mkfs.ext4 interactively asks for confirmation, -F disables that behavior
            const auto forceFlag = Partitions[i - 1] == L"ext4" ? L" -F " : L"";
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"mkfs." + Partitions[i - 1] + forceFlag + L" " + partition), (DWORD)0);
        }
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhdTest)
        {
            WaitForDiskReady();
        }
    }

    void ValidateMountPoint(
        const std::wstring& BlockDevice,
        const std::optional<std::wstring>& Mountpoint,
        const std::optional<std::wstring>& ExpectedOption = {},
        const std::optional<std::wstring>& ExpectedType = {})
    {
        auto mount = GetBlockDeviceMount(BlockDevice);
        if (Mountpoint.has_value())
        {
            VERIFY_IS_TRUE(mount.has_value());
        }
        else
        {
            VERIFY_IS_FALSE(mount.has_value());
            return;
        }

        VERIFY_ARE_EQUAL(mount.value()[1], Mountpoint.value());
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -d " + Mountpoint.value()), (DWORD)0);

        // If specified, validate that ExpectedOption is in the mount options
        // (We don't want to do a direct compare because the kernel might add some like rw, ...)
        if (ExpectedOption.has_value())
        {
            VERIFY_ARE_NOT_EQUAL(mount.value()[3].find(ExpectedOption.value()), std::string::npos);
        }

        // If specified, validate the filesystem
        if (ExpectedType.has_value())
        {
            VERIFY_ARE_EQUAL(mount.value()[2], ExpectedType.value());
        }
    }

    void TestBareMountImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        WslKeepAlive keepAlive;

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        if (isVhd)
        {
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
        }
        else
        {
            ValidateOffline(false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
            ValidateOffline(true);
        }

        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        VERIFY_IS_FALSE(GetBlockDeviceMount(disk).has_value());

        ValidateDiskState({deviceName, {}}, keepAlive);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhd)
        {
            ValidateOffline(false);
        }
    }

    void TestMountOnePartitionImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, isVhd);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);
        auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);
        auto mountTarget = L"/mnt/wsl/" + trimmedDiskName + L"p1";

        ValidateMountPoint(disk + L"1", mountTarget);

        ValidateDiskState({deviceName, {{1, {}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);
        WaitForDiskReady();

        if (!isVhd)
        {
            ValidateOffline(false);
        }

        // Validate that the mount folder was deleted
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -e " + mountTarget), (DWORD)1);

        // Mount the same partition, but with a specific mount option
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1 --options \"data=ordered\""), (DWORD)0);

        // Validate that the mount option was properly passed
        disk = GetBlockDeviceInWsl();
        ValidateMountPoint(disk + L"1", mountTarget, L"data=ordered");
        ValidateDiskState({deviceName, {{1, {}, L"data=ordered"}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);
        WaitForDiskReady();

        if (!isVhd)
        {
            ValidateOffline(false);
        }
    }

    void TestMountTwoPartitionsImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition and one fat partitions
        FormatDisk({L"ext4", L"vfat"}, isVhd);

        // Mount then both
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 2 --type vfat"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, L"ext4");
        ValidateMountPoint(disk + L"2", L"/mnt/wsl/" + trimmedDiskName + L"p2", {}, L"vfat");
        ValidateDiskState({deviceName, {{1, {}, {}}, {2, {L"vfat"}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);
        WaitForDiskReady();

        if (!isVhd)
        {
            ValidateOffline(false);
        }
    }

    void TestAttachThenMountImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        FormatDisk({L"ext4"}, isVhd);

        // Mount then both
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);

        ValidateDiskState({deviceName, {{1, {}, {}}}}, keepAlive);

        // Validate that our disk is still mounted
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, {});

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);
    }

    void TestMountWholeDiskImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Format the volume as ext4
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"mkfs.ext4 -F " + disk), (DWORD)0);

        // Then mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --type ext4"), (DWORD)0);

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);
        auto mountTarget = L"/mnt/wsl/" + trimmedDiskName;
        ValidateMountPoint(disk, mountTarget, {}, L"ext4");
        ValidateDiskState({deviceName, {{0, {L"ext4"}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhd)
        {
            WaitForDiskReady();
            ValidateOffline(false);
        }
    }

    void TestMountStateIsDeletedOnShutdownImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition
        FormatDisk({L"ext4"}, isVhd);

        // Mount it
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1 --type ext4"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        ValidateDiskState({deviceName, {{1, {L"ext4"}, {}}}}, keepAlive);
        keepAlive.Reset();

        // wsl --shutdown clears any disk state
        WslShutdown();

        if (!isVhd)
        {
            ValidateOffline(false);
        }

        // No state should be left in registry
        const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(User->User.Sid);
        VERIFY_ARE_EQUAL(wsl::windows::common::registry::EnumKeys(key.get(), KEY_READ).size(), 0);
    }

    void TestFilesystemDetectionWholeDiskImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Format the volume as fat
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"mkfs.fat --mbr=no -I " + disk), (DWORD)0);

        // Then mount it. The filesystem should be autodetected
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand), (DWORD)0);

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);
        auto mountTarget = L"/mnt/wsl/" + trimmedDiskName;
        ValidateMountPoint(disk, mountTarget, {}, L"vfat");
        ValidateDiskState({deviceName, {{0, {}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhd)
        {
            WaitForDiskReady();
            ValidateOffline(false);
        }
    }

    void TestMountTwoPartitionsWithDetectionImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Create a MBR disk with 1 ext4 partition and one fat partitions
        FormatDisk({L"ext4", L"vfat"}, isVhd);

        // Mount then both (filesystems should be detected).
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 1"), (DWORD)0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --partition 2"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));

        // Validate that the mount succeeded
        std::wstring trimmedDiskName(deviceName);
        Trim(trimmedDiskName);

        ValidateMountPoint(disk + L"1", L"/mnt/wsl/" + trimmedDiskName + L"p1", {}, L"ext4");
        ValidateMountPoint(disk + L"2", L"/mnt/wsl/" + trimmedDiskName + L"p2", {}, L"vfat");
        ValidateDiskState({deviceName, {{1, {}, {}}, {2, {}, {}}}}, keepAlive);

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhd)
        {
            WaitForDiskReady();
            ValidateOffline(false);
        }
    }

    void TestFilesystemDetectionFailImpl(bool isVhd)
    {
        WSL2_TEST_ONLY();

        const auto deviceName = (isVhd) ? VhdDevice : DiskDevice;
        const auto mountCommand = (isVhd) ? (L"--mount " + deviceName + L" --vhd") : (L"--mount " + deviceName);

        WslKeepAlive keepAlive;

        // Write zeroes in the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(mountCommand + L" --bare"), (DWORD)0);
        const auto disk = GetBlockDeviceInWsl();
        VERIFY_IS_TRUE(IsBlockDevicePresent(disk));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"dd bs=4M count=1 if=/dev/zero of=" + disk), (DWORD)0);

        // Then try to mount it
        wsl::windows::common::SvcComm service;
        const auto result = service.MountDisk(
            deviceName.c_str(), isVhd ? LXSS_ATTACH_MOUNT_FLAGS_VHD : LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH, 0, nullptr, nullptr, nullptr);

        // Validate that the mount fail because the filesystem couldn't be detected
        VERIFY_ARE_EQUAL(result.Result, -1); //-EINVAL
        VERIFY_ARE_EQUAL(result.Step, 6);    // LxMiniInitMountStepDetectFilesystem

        // Unmount the disk
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unmount " + deviceName), (DWORD)0);

        if (!isVhd)
        {
            WaitForDiskReady();
            ValidateOffline(false);
        }
    }
};
} // namespace MountTests