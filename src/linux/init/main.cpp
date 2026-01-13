/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entrypoint of the WSL init implementation.

--*/

#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/audit.h> /* Definition of AUDIT_* constants */
#include <linux/if_tun.h>
#include <linux/loop.h>
#include <linux/net.h>
#include <linux/random.h>
#include <linux/vm_sockets.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/sock_diag.h>
#include <sys/utsname.h>
#include <linux/netlink.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>
#include <assert.h>
#include "configfile.h"
#include "lxfsshares.h"
#include "common.h"
#include "util.h"
#include "seccomp_defs.h"
#include "mountutilcpp.h"
#include "message.h"
#include "binfmt.h"
#include "address.h"
#include "SocketChannel.h"

#define BSDTAR_PATH "/usr/bin/bsdtar"
#define BINFMT_REGISTER_STRING ":" LX_INIT_BINFMT_NAME ":M::MZ::" LX_INIT_PATH ":FP\n"
#define BINFMT_PATH PROCFS_PATH "/sys/fs/binfmt_misc"
#define CHRONY_CONF_PATH ETC_PATH "/chrony.conf"
#define CHRONYD_PATH "/sbin/chronyd"
#define CROSS_DISTRO_SHARE_PATH "/mnt/wsl"
#define DEVFS_PATH "/dev"
#define DEVNULL_PATH DEVFS_PATH "/null"
#define DHCPCD_CONF_PATH "/dhcpcd.conf"
#define DHCPCD_PATH "/usr/sbin/dhcpcd"

#define DISTRO_PATH "/distro"
#define ETC_PATH "/etc"
#define GPU_SHARE_PREFIX "/gpu_"
#define GPU_SHARE_DRIVERS GPU_SHARE_PREFIX LXSS_GPU_DRIVERS_SHARE
#define GPU_SHARE_LIB GPU_SHARE_PREFIX LXSS_GPU_LIB_SHARE
#define GPU_SHARE_LIB_INBOX GPU_SHARE_LIB "_inbox"
#define GPU_SHARE_LIB_PACKAGED GPU_SHARE_LIB "_packaged"
#define KERNEL_MODULES_PATH "/lib/modules"
#define KERNEL_MODULES_VHD_PATH "/modules"
#define KERNEL_MODULES_OVERLAY "/modules_overlay"
#define MODPROBE_PATH "/sbin/modprobe"
#define PROCFS_PATH "/proc"
#define RESOLV_CONF_FILE "resolv.conf"
#define RESOLV_CONF_PATH ETC_PATH "/" RESOLV_CONF_FILE
#define RECLAIM_PATH "/sys/fs/cgroup/memory.reclaim"
#define SCSI_DEVICE_PATH "/sys/bus/scsi/devices"
#define SCSI_DEVICE_NAME_PREFIX "0:0:0:"
#define SCSI_DEVICE_PREFIX SCSI_DEVICE_PATH "/" SCSI_DEVICE_NAME_PREFIX
#define SYSFS_PATH "/sys"
#define SYSTEM_DISTRO_PATH "/system"
#define SYSTEM_DISTRO_VHD_PATH "/systemvhd"
#define WSLG_PATH "/wslg"

#define syscall_arg(_n) (offsetof(struct seccomp_data, args[_n]))
#define syscall_nr (offsetof(struct seccomp_data, nr))
#define syscall_arch (offsetof(struct seccomp_data, arch))

constexpr auto c_trueString = "1";

struct VmConfiguration
{
    bool EnableGpuSupport = false;
    bool EnableGuiApps = false;
    bool EnableInboxGpuLibs = false;
    bool EnableSafeMode = false;
    bool EnableSystemDistro = false;
    bool EnableCrashDumpCollection = false;
    std::string KernelModulesPath;
    LX_MINI_INIT_NETWORKING_MODE NetworkingMode = LxMiniInitNetworkingModeNone;
};

int g_LogFd = STDERR_FILENO;
int g_TelemetryFd = -1;
std::optional<bool> g_EnableSocketLogging;

int Chroot(const char* Target);

void ConfigureMemoryReduction(int PageReportingOrder, LX_MINI_INIT_MEMORY_RECLAIM_MODE Mode);

void CreateSwap(unsigned int Lun);

int CreateTempDirectory(const char* ParentPath, std::string& Path);

int DetachScsiDisk(unsigned int Lun);

int EjectScsi(unsigned int Lun);

int EnableInterface(int Socket, const char* Name);

int ExportToSocket(const char* Source, int Socket, int ErrorSocket, unsigned int flags);

int FormatDevice(unsigned int Lun);

std::string GetLunDeviceName(unsigned int Lun);

std::string GetLunDevicePath(unsigned int Lun);

int GetDiskPartitionIndex(const char* DiskPath, const char* PartitionName);

std::string GetMountTarget(const char* Name);

long long int GetUserCpuTime(void);

ssize_t GetMemoryInUse(void);

int ImportFromSocket(const char* Destination, int Socket, int ErrorSocket, unsigned int Flags);

int Initialize(const char* Hostname);

void InjectEntropy(gsl::span<gsl::byte> EntropyBuffer);

void LaunchInit(
    int SocketFd,
    const char* Target,
    bool EnableGuiApps,
    const VmConfiguration& Config,
    const char* VmId = nullptr,
    const char* DistributionName = nullptr,
    const char* SharedMemoryRoot = nullptr,
    const char* InstallPath = nullptr,
    const char* UserProfile = nullptr,
    std::optional<pid_t> DistroInitPid = {});

void LaunchSystemDistro(
    int SocketFd,
    const char* Target,
    const VmConfiguration& Config,
    const char* VmId,
    const char* DistributionName,
    const char* SharedMemoryRoot,
    const char* InstallPath,
    const char* UserProfile,
    pid_t DistroInitPid);

std::map<unsigned long, std::string> ListDiskPartitions(const std::string& DeviceName, std::optional<unsigned long> WaitForIndex = {});

std::vector<unsigned int> ListScsiDisks();

void LogException(const char* Message, const char* Description) noexcept;

int MountDevice(LX_MINI_INIT_MOUNT_DEVICE_TYPE DeviceType, unsigned int DeviceId, const char* Target, const char* FsType, unsigned int Flags, const char* Options);

int MountSystemDistro(LX_MINI_INIT_MOUNT_DEVICE_TYPE DeviceType, unsigned int DeviceId);

int MountInit(const char* Target);

int MountPlan9(const char* Name, const char* Target, bool ReadOnly, std::optional<int> BufferSize = {});

int ProcessMessage(wsl::shared::SocketChannel& channel, LX_MESSAGE_TYPE Type, gsl::span<gsl::byte> Buffer, VmConfiguration& Config);

wil::unique_fd RegisterSeccompHook();

int ReportMountStatus(wsl::shared::SocketChannel& Channel, int Result, LX_MINI_MOUNT_STEP Step);

int SendCapabilities(wsl::shared::SocketChannel& Channel);

int SetCloseOnExec(int Fd, bool Enable);

int SetEphemeralPortRange(uint16_t Start, uint16_t End);

void StartDebugShell();

int StartDhcpClient(int DhcpTimeout);

int StartGuestNetworkService(int GnsFd, wil::unique_fd&& DnsTunnelingFd, uint32_t DnsTunnelingIpAddress);

void StartPortTracker(LX_MINI_INIT_PORT_TRACKER_TYPE Type);

void StartTimeSyncAgent(void);

void WaitForBlockDevice(const char* Path);

int WaitForChild(pid_t Pid, const char* Name);

int Chroot(const char* Target)

/*++

Routine Description:

    This routine changes the root directory of the calling process to the specified
    path.

Arguments:

    Target - Supplies the path to chroot to.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Set the current working directory to the distro mount point, move the
    // mount to the root, and chroot.
    //

    if (chdir(Target) < 0)
    {
        LOG_ERROR("chdir({}) failed {}", Target, errno);
        return -1;
    }

    if (mount(".", "/", nullptr, MS_MOVE, nullptr) < 0)
    {
        LOG_ERROR("mount(MS_MOVE) failed {}", errno);
        return -1;
    }

    if (chroot(".") < 0)
    {
        LOG_ERROR("chroot failed {}", errno);
        return -1;
    }

    return 0;
}

void ConfigureMemoryReduction(int PageReportingOrder, LX_MINI_INIT_MEMORY_RECLAIM_MODE Mode)

/*++

Routine Description:

    This routine sets the page reporting order.

Arguments:

    PageReportingOrder - Supplies the page reporting order. This value determines the size of cold discard hints
        by using the equation: 2^PageReportingOrder * PAGE_SIZE
        Example: 2^9 * 4096 = 2MB

    Mode - Supplies the memory reclaim mode.

Return Value:

    None.

--*/

try
{
    //
    // Ensure the value falls within a reasonable range (single page to 2MB).
    //

    if (PageReportingOrder < 0 || PageReportingOrder > 9)
    {
        LOG_WARNING("Invalid page_reporting_order {}", PageReportingOrder);
        PageReportingOrder = 0;
    }
    else
    {
        WriteToFile("/sys/module/page_reporting/parameters/page_reporting_order", std::to_string(PageReportingOrder).c_str());
    }

    //
    // Create a worker thread to periodically check if the VM is idle and performs memory compaction.
    // This ensures that the maximum number of pages can be discarded to the host.
    //
    // N.B. Compaction is not needed if page reporting order is set to single page mode.
    //

    if (PageReportingOrder == 0 && Mode == LxMiniInitMemoryReclaimModeDisabled)
    {
        return;
    }

    std::thread([PageReportingOrder = PageReportingOrder, Mode = Mode]() mutable {
        try
        {
            //
            // Set the thread's scheduling policy to idle.
            //

            sched_param Parameter{};
            Parameter.sched_priority = 0;
            THROW_LAST_ERROR_IF(pthread_setschedparam(pthread_self(), SCHED_IDLE, &Parameter) < 0);

            //
            // Periodically check if the machine is idle by querying procfs for CPU usage.
            // Memory compaction will occur if both of the following conditions are true:
            //     1. The CPU time since the last check is greater than the idle threshold.
            //     2. The current CPU usage is below the idle threshold. This is measured by taking two readings one second apart.
            //

            double MemoryLow = 1024 * 1024 * 1024;
            double MemoryHigh = 1.1 * 1024.0 * 1024.0 * 1024.0;
            const int IdleThreshold = get_nprocs(); // Change math to adjust if sysconf(_SC_CLK_TCK) != 100? Is 1%
            long long int Start, Stop = 0;
            auto constexpr SleepDuration = std::chrono::seconds(30);
            size_t ReclaimIndex = 0;
            long long int const ReclaimThreshold = (get_nprocs() * sysconf(_SC_CLK_TCK) * SleepDuration / std::chrono::seconds(1)) / 200; // 0.5%
            long long int ReclaimWindow[20] = {}; // 10 minutes
            long long int ReclaimWindowLength = COUNT_OF(ReclaimWindow);
            bool ReclaimIdling;

            //
            // Fall back to drop cache if the required cgroup path is not present.
            //

            if (Mode == LxMiniInitMemoryReclaimModeGradual && access(RECLAIM_PATH, W_OK) < 0)
            {
                LOG_WARNING("access({}, W_OK) failed {}, falling back to autoMemoryReclaim = dropcache", RECLAIM_PATH, errno);
                Mode = LxMiniInitMemoryReclaimModeDropCache;
            }

            if (Mode == LxMiniInitMemoryReclaimModeGradual)
            {
                static_assert(COUNT_OF(ReclaimWindow) >= 6);
                ReclaimWindowLength = 6; // Set to 3 minutes.
            }

            for (auto i = 1; i < ReclaimWindowLength; i++)
            {
                ReclaimWindow[i] = LLONG_MIN;
            }

            std::this_thread::sleep_for(SleepDuration);
            for (;;)
            {
                auto const Target = std::chrono::steady_clock::now() + SleepDuration;
                Start = GetUserCpuTime();
                THROW_LAST_ERROR_IF(Start == -1);

                if (Mode != LxMiniInitMemoryReclaimModeDisabled)
                {
                    //
                    // Ensure that utilization is below 0.5% from the last 30 seconds, and last n minutes, of usage.
                    //

                    size_t const LastIndex = (ReclaimIndex + 1) % ReclaimWindowLength;
                    if ((ReclaimWindow[LastIndex] > Start - ReclaimThreshold * (ReclaimWindowLength + 1)) &&
                        (ReclaimWindow[ReclaimIndex] > Start - ReclaimThreshold))
                    {
                        if (Mode == LxMiniInitMemoryReclaimModeGradual)
                        {
                            double MemorySize = GetMemoryInUse();
                            THROW_LAST_ERROR_IF(MemorySize < 0);

                            if (MemorySize > MemoryHigh)
                            {
                                ReclaimIdling = false;
                            }

                            if (!ReclaimIdling && MemorySize > MemoryLow)
                            {
                                double MemoryTargetSize = MemorySize * 0.97;
                                std::string MemoryToFree = std::to_string(size_t(MemorySize - MemoryTargetSize));
                                // EAGAIN Means that it attempted, but was unable to evict sufficient pages.
                                THROW_LAST_ERROR_IF(WriteToFile(RECLAIM_PATH, MemoryToFree.c_str()) < 0 && errno != EAGAIN);

                                if (MemoryTargetSize < MemoryLow)
                                {
                                    ReclaimIdling = true;
                                }
                            }
                        }
                        else if (!ReclaimIdling)
                        {
                            ReclaimIdling = true;
                            THROW_LAST_ERROR_IF(WriteToFile(PROCFS_PATH "/sys/vm/drop_caches", "1\n") < 0);
                        }
                    }
                    else
                    {
                        ReclaimIdling = false;
                    }

                    ReclaimIndex = LastIndex;
                    ReclaimWindow[ReclaimIndex] = Start;
                }

                //
                // Perform memory compaction if the VM is idle.
                //
                // N.B. Memory compaction is not needed if the page reporting order is set to single page (0).
                //

                if (PageReportingOrder != 0 && (Start - Stop) > IdleThreshold)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    const long long int Stop = GetUserCpuTime();
                    THROW_LAST_ERROR_IF(Stop == -1);
                    if ((Stop - Start) < IdleThreshold)
                    {
                        THROW_LAST_ERROR_IF(WriteToFile(PROCFS_PATH "/sys/vm/compact_memory", "1\n") < 0);
                    }
                }

                std::this_thread::sleep_until(Target);
            }
        }
        CATCH_LOG()
    }).detach();
}
CATCH_LOG()

wil::unique_fd CreateNetlinkSocket(void)

/*++

Routine Description:

    Create and bind a netlink socket.

Arguments:

    None.

Return Value:

    The socket file descriptor or < 0 on failure.

--*/

{
    wil::unique_fd Fd{socket(AF_NETLINK, SOCK_RAW, NETLINK_SOCK_DIAG)};
    if (!Fd)
    {
        LOG_ERROR("socket failed {}", errno);
        return {};
    }

    struct sockaddr_nl Address;
    Address.nl_family = AF_NETLINK;
    if (bind(Fd.get(), (struct sockaddr*)&Address, sizeof(Address)) < 0)
    {
        LOG_ERROR("bind failed {}", errno);
        return {};
    }

    return Fd;
}

void CreateSwap(unsigned int Lun)

/*++

Routine Description:

    This routine sets up a swap area on the specified SCSI device.

Arguments:

    Lun - Supplies the LUN number of the SCSI device.

Return Value:

    None.

--*/

{
    //
    // Create the swap file asynchronously using the mkswap and swapon utilities in the system distro.
    //
    // N.B. This is done because creating the swap file can take some time and
    //      the swap file does not need to be available immediately.
    //

    UtilCreateChildProcess("CreateSwap", [Lun]() {
        std::string DevicePath = GetLunDevicePath(Lun);

        WaitForBlockDevice(DevicePath.c_str());

        std::string CommandLine = std::format("/usr/sbin/mkswap '{}'", DevicePath);
        THROW_LAST_ERROR_IF(UtilExecCommandLine(CommandLine.c_str(), nullptr) < 0);

        CommandLine = std::format("/usr/sbin/swapon '{}'", DevicePath);
        UtilExecCommandLine(CommandLine.c_str(), nullptr);
    });
}

int CreateTempDirectory(const char* ParentPath, std::string& Path)

/*++

Routine Description:

    This routine creates a unique directory under the specified parent path.

Arguments:

    ParentPath - Supplies the path of the parent directory.

    Path - Supplies a buffer to receive the path of the child directory that was
        created.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if (ParentPath)
    {
        Path = ParentPath;
    }

    //
    // Generate a random name for the directory.
    //
    // N.B. mkdtemp requires a template string that ends in "XXXXXX".
    //

    Path += "/wslXXXXXX";

    if (mkdtemp(Path.data()) == NULL)
    {
        LOG_ERROR("mkdtemp({}) failed {}", Path.c_str(), errno);
        return -1;
    }

    return 0;
}

dev_t GetBlockDeviceNumber(const std::string& BlockDeviceName)

/*++

Routine Description:

    This method return the device number of a given block device.

Arguments:

    BlockDeviceName - Supplies the name of the block device.

Return Value:

    The device block number. Throws on error.

--*/

{
    std::string content = wsl::shared::string::ReadFile<char, char>(std::format("/sys/block/{}/dev", BlockDeviceName).c_str());
    auto separator = content.find(':');

    if (separator == 0 || separator - 1 >= content.size() || separator == std::string::npos)
    {
        LOG_ERROR("Failed to parse device number '{}' for device '{}'", content.c_str(), BlockDeviceName.c_str());
        THROW_ERRNO(EINVAL);
    }

    try
    {
        return makedev(std::strtoul(content.c_str(), nullptr, 10), std::strtoul(content.substr(separator + 1).c_str(), nullptr, 10));
    }
    catch (...)
    {
        LOG_ERROR("Failed to parse device number '{}' for device '{}'", content.c_str(), BlockDeviceName.c_str());
        THROW_ERRNO(EINVAL);
    }
}

int DetachScsiDisk(unsigned int Lun)

/*++

Routine Description:

    This routine detaches a SCSI disk.

Arguments:

    Lun - Supplies the LUN of the disk to detach.

Return Value:

    0 on success, -1 on failure.

--*/

{
    auto deviceName = GetLunDeviceName(Lun);

    try
    {
        auto deviceNumbers = std::set<dev_t>{GetBlockDeviceNumber(deviceName)};
        for (const auto& e : ListDiskPartitions(deviceName.c_str()))
        {
            deviceNumbers.insert(GetBlockDeviceNumber(std::format("{}/{}", deviceName, e.second)));
        }

        mountutil::MountEnum mounts;
        while (mounts.Next())
        {
            if (deviceNumbers.find(mounts.Current().Device) != deviceNumbers.end())
            {
                if (umount(mounts.Current().MountPoint) < 0)
                {
                    LOG_ERROR("Failed to unmount '{}', {}", mounts.Current().MountPoint, errno);
                }
            }
        }
    }
    CATCH_LOG();

    // Flush the block device.
    std::string DevicePath = DEVFS_PATH + std::string("/") + deviceName;
    wil::unique_fd BlockDevice{open(DevicePath.c_str(), O_RDONLY)};
    int Result = ioctl(BlockDevice.get(), BLKFLSBUF);
    if (Result < 0)
    {
        LOG_ERROR("Failed to flush block device: '{}', {}", DevicePath.c_str(), errno);
        return Result;
    }

    // Close the device before trying to delete it.
    BlockDevice.reset();

    // Remove the block device.
    return WriteToFile(std::format("/sys/block/{}/device/delete", deviceName).c_str(), "1");
}

int DetectFilesystem(const char* BlockDevice, std::string& Output)

/*++

Routine Description:

    This routine performs file system detect on a block device.

Arguments:

    BlockDevice - Path to the block device.

    Output - Detected filesystem, if any.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    //
    // Wait for the block device to be available.
    //

    wsl::shared::retry::RetryWithTimeout<void>(
        [&]() { THROW_LAST_ERROR_IF(!wil::unique_fd{open(BlockDevice, O_RDONLY)}); },
        c_defaultRetryPeriod,
        c_defaultRetryTimeout,
        []() {
            auto err = wil::ResultFromCaughtException();
            return err == ENOENT || err == ENXIO;
        });

    auto CommandLine = std::format("/usr/sbin/blkid '{}' -p -s TYPE -o value -u filesystem", BlockDevice);
    if (UtilExecCommandLine(CommandLine.c_str(), &Output) < 0)
    {
        return -1;
    }

    while (!Output.empty() && Output.back() == '\n')
    {
        Output.pop_back();
    }

    LOG_INFO("Detected {} filesystem for device: {}", Output, BlockDevice);
    return 0;
}
CATCH_RETURN_ERRNO()

int EjectScsi(unsigned int Lun)

/*++

Routine Description:

    This routine ejects the specified SCSI device.

Arguments:

    Lun - Supplies the LUN of the SCSI device to eject.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // Perform a sync to ensure all writes are flushed.
    //

    sync();

    //
    // Write "1" to /sys/bus/scsi/devices/0:0:<controller>:<lun>/delete to eject the SCSI device.
    //

    std::string Path = std::format("{}{}/delete", SCSI_DEVICE_PREFIX, Lun);
    if (WriteToFile(Path.c_str(), c_trueString) < 0)
    {
        return -1;
    }

    return 0;
}
CATCH_RETURN_ERRNO()

void EnableCrashDumpCollection()
{
    if (symlink("/init", "/" LX_INIT_WSL_CAPTURE_CRASH) < 0)
    {
        LOG_ERROR("symlink({}, {}) failed {}", "/init", "/" LX_INIT_WSL_CAPTURE_CRASH, errno);
        return;
    }

    // If the first character is a pipe, then the kernel will interpret this path as a command.
    constexpr auto core_pattern = "|/" LX_INIT_WSL_CAPTURE_CRASH " %t %E %p %s";
    WriteToFile("/proc/sys/kernel/core_pattern", core_pattern);
}

int EnableInterface(int Socket, const char* Name)

/*++

Routine Description:

    This routine marks the specified interface as up / running.

Arguments:

    Socket - Supplies a socket file descriptor.

    Name - Supplies the name of an interface.

Return Value:

    0 on success, -1 on failure.

--*/

{
    ifreq InterfaceRequest{};
    strncpy(InterfaceRequest.ifr_name, Name, IFNAMSIZ - 1);
    if (ioctl(Socket, SIOCGIFFLAGS, &InterfaceRequest) < 0)
    {
        LOG_ERROR("SIOCGIFFLAGS failed {}", errno);
        return -1;
    }

    InterfaceRequest.ifr_flags |= (IFF_UP | IFF_RUNNING);
    if (ioctl(Socket, SIOCSIFFLAGS, &InterfaceRequest) < 0)
    {
        LOG_ERROR("SIOCSIFFLAGS failed {}", errno);
        return -1;
    }

    return 0;
}

int ExportToSocket(const char* Source, int Socket, int ErrorSocket, unsigned int Flags)

/*++

Routine Description:

    This routine uses bsdtar to export a source directory in tar format via a
    socket.

Arguments:

    Source - Supplies the path to export.

    Socket - Supplies the socket to write to.

    Flags - Additional compression flags.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Create a child process running bsdtar with the socket set to stdout.
    //

    int ChildPid = UtilCreateChildProcess("ExportDistro", [Source, TarFd = Socket, ErrorSocket = ErrorSocket, Flags = Flags]() {
        THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(TarFd, STDOUT_FILENO)) < 0);
        THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(ErrorSocket, STDERR_FILENO)) < 0);

        std::string compressionArguments;

        if (WI_IsFlagSet(Flags, LxMiniInitMessageFlagExportCompressGzip))
        {
            assert(!WI_IsFlagSet(Flags, LxMiniInitMessageFlagExportCompressXzip));

            compressionArguments = "-cz";
        }
        else if (WI_IsFlagSet(Flags, LxMiniInitMessageFlagExportCompressXzip))
        {
            compressionArguments = "-cJ";
        }
        else
        {
            compressionArguments = "-c";
        }

        if (WI_IsFlagSet(Flags, LxMiniInitMessageFlagVerbose))
        {
            compressionArguments += "vv";
        }

        std::vector<const char*> arguments{
            BSDTAR_PATH,
            "-C",
            Source,
            compressionArguments.c_str(),
            "--one-file-system",
            "--xattrs",
            "--numeric-owner",
            "-f",
            "-",
            ".",
            nullptr};

        if (WI_IsFlagSet(Flags, LxMiniInitMessageFlagVerbose))
        {
            arguments.emplace(arguments.begin() + 3, "--totals");
        }

        execv(BSDTAR_PATH, const_cast<char**>(arguments.data()));
        LOG_ERROR("execl failed, {}", errno);
    });

    if (ChildPid < 0)
    {
        return -1;
    }

    //
    // Wait for the child to exit and shut down the socket.
    //

    const int Result = WaitForChild(ChildPid, BSDTAR_PATH);
    if (shutdown(Socket, SHUT_WR) < 0)
    {
        LOG_ERROR("shutdown failed {}", errno);
    }

    return Result;
}

int FormatDevice(unsigned int Lun)

/*++

Routine Description:

    This routine formats the specified SCSI device with the ext4 file system.
    N.B. The group size was chosen based on the best practices for Linux VHDs:
         https://docs.microsoft.com/en-us/windows-server/virtualization/hyper-v/best-practices-for-running-linux-on-hyper-v

Arguments:

    Lun - Supplies the LUN number of the SCSI device.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    std::string DevicePath = GetLunDevicePath(Lun);

    WaitForBlockDevice(DevicePath.c_str());

    std::string CommandLine = std::format("/usr/sbin/mkfs.ext4 -G 4096 '{}'", DevicePath);
    if (UtilExecCommandLine(CommandLine.c_str(), nullptr) < 0)
    {
        return -1;
    }

    return 0;
}
CATCH_RETURN_ERRNO()

std::string GetLunDeviceName(unsigned int Lun)

/*++

Routine Description:

    This routine returns the device name(sdX) for the specified SCSI device.

Arguments:

Lun - Supplies a SCSI LUN.

Return Value:

    The device name (throws on error).

--*/

{
    //
    // Construct a path to the block directory which contains a single directory
    // entry with the name of the device where the vhd is attached, for example: sda.
    //
    // N.B. A retry loop is needed because there is a delay between when the vhd
    //      is hot-added from the host, and when the sysfs directory is
    //      available in the guest.
    //

    std::string Path = std::format("{}{}/block", SCSI_DEVICE_PREFIX, Lun);
    return wsl::shared::retry::RetryWithTimeout<std::string>(
        [&]() {
            wil::unique_dir Dir{opendir(Path.c_str())};
            THROW_LAST_ERROR_IF(!Dir);

            //
            // Find the first directory entry that does not begin with a dot.
            //

            dirent64* Entry{};
            while ((Entry = readdir64(Dir.get())) != nullptr)
            {
                if (Entry->d_name[0] != '.')
                {
                    return std::string(Entry->d_name);
                }
            }

            THROW_ERRNO(ENXIO);
        },
        c_defaultRetryPeriod,
        c_defaultRetryTimeout);
}

std::string GetLunDevicePath(unsigned int Lun)

/*++

Routine Description:

    This routine returns the device path for the specified SCSI device.

Arguments:

    Lun - Supplies a SCSI LUN.

Return Value:

    The device path;

--*/

{
    auto DeviceName = GetLunDeviceName(Lun);

    return std::format("{}/{}", DEVFS_PATH, DeviceName.c_str());
}

int GetDiskPartitionIndex(const char* DiskPath, const char* PartitionName)

/*++

Routine Description:

    Finds the partition number of a specified partition path.

Arguments:

    DiskPath - Supplies the path to the Disk (ex: /sys/block/sda).

    PartitionName - Supplies the partition name (ex: sda1).

Return Value:

    > 0 (the partition number), < 0 on failure.

--*/

try
{
    std::string FilePath = std::format("{}/{}/partition", DiskPath, PartitionName);
    wil::unique_fd Fd{open(FilePath.c_str(), O_RDONLY)};
    if (!Fd)
    {
        LOG_ERROR("open({}) failed {}", FilePath, errno);
        return -errno;
    }

    char Buffer[64];
    int Result = TEMP_FAILURE_RETRY(read(Fd.get(), Buffer, (sizeof(Buffer) - 1)));
    if (Result < 0)
    {
        LOG_ERROR("read failed {}", errno);
        return -errno;
    }

    Buffer[Result] = '\0';
    return atol(Buffer);
}
CATCH_RETURN_ERRNO()

long long int GetUserCpuTime(void)

/*++

Routine Description:

    This routine parses /proc/stat to query a summary of all user CPU time.

Arguments:

    None.

Return Value:

    The current user CPU counter for all cores.

--*/

{
    wil::unique_fd Fd{open(PROCFS_PATH "/stat", O_RDONLY)};
    if (!Fd)
    {
        LOG_ERROR("open failed {}", errno);
        return -1;
    }

    char Buffer[32];
    int Result = TEMP_FAILURE_RETRY(read(Fd.get(), Buffer, (sizeof(Buffer) - 1)));
    if (Result < 0)
    {
        LOG_ERROR("read failed {}", errno);
        return -1;
    }

    //
    // Parse the first line of /proc/stat which is in the format
    // "cpu  <counter>".
    //

    Buffer[Result] = '\0';
    char* Sp1;
    char* Info = strtok_r(Buffer, " \n", &Sp1);
    Info = strtok_r(nullptr, " \n", &Sp1);
    return strtoll(Info, nullptr, 10);
}

ssize_t GetMemoryInUse(void)

/*++

Routine Description:

    This routine returns the amount memory in use in bytes.

Arguments:

    None.

Return Value:

    Total memory - Free memory. Includes that used by cache and buffers.

--*/
try
{
    struct sysinfo Info = {};
    THROW_LAST_ERROR_IF(sysinfo(&Info) < 0);
    return Info.totalram - Info.freeram;
}
CATCH_RETURN_ERRNO()

int ImportFromSocket(const char* Destination, int Socket, int ErrorSocket, unsigned int Flags)

/*++

Routine Description:

    This routine uses bsdtar to extract a tar file via a socket.

Arguments:

    Destination - Supplies the path to extract the tar.

    Socket - Supplies the socket to read from.

    Flags - Import flags.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Create a child process running bsdtar with the socket set to stdin.
    //

    int ChildPid = UtilCreateChildProcess("ImportDistro", [Destination, TarFd = Socket, ErrorSocket = ErrorSocket, Flags]() {
        THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(TarFd, STDIN_FILENO)) < 0);
        THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(ErrorSocket, STDERR_FILENO)) < 0);

        execl(
            BSDTAR_PATH,
            BSDTAR_PATH,
            "-C",
            Destination,
            "-x",
            WI_IsFlagSet(Flags, LxMiniInitMessageFlagVerbose) ? "-vvp" : "-p",
            "--xattrs",
            "--numeric-owner",
            "-f",
            "-",
            NULL);
        LOG_ERROR("execl failed, {}", errno);
    });

    if (ChildPid < 0)
    {
        return -1;
    }

    return WaitForChild(ChildPid, BSDTAR_PATH);
}

void StartDebugShell()

/*++

Routine Description:

    This routine starts the debug shell.

Arguments:

    None.

Return Value:

    None.

--*/

{
    // Spawn a child process to handle relaunching the debug shell if it exits.
    UtilCreateChildProcess("DebugShell", []() {
        for (;;)
        {
            const auto Pid = UtilCreateChildProcess("agetty", []() {
                execl("/usr/bin/setsid", "/usr/bin/setsid", "/sbin/agetty", "-w", "-L", LX_INIT_HVC_DEBUG_SHELL, "-a", "root", NULL);
                LOG_ERROR("execl failed, {}", errno);
            });

            if (Pid < 0)
            {
                _exit(1);
            }

            int Status = -1;
            if (TEMP_FAILURE_RETRY(waitpid(Pid, &Status, 0)) < 0)
            {
                LOG_ERROR("waitpid failed {}", errno);
                _exit(1);
            }
        }
    });
}

int StartDhcpClient(int DhcpTimeout)

/*++

Routine Description:

    Starts the dhcp client daemon. Blocks until the initial DHCP lease is acquired,
    then the daemon continues running in the background to handle renewals.

Arguments:

    DhcpTimeout - Supplies the timeout in seconds for the DHCP request.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    int ChildPid = UtilCreateChildProcess("dhcpcd", [DhcpTimeout]() {
        //
        // Write the dhcpcd.conf config file.
        //

        std::string Config = std::format(
            "option subnet_mask, routers, broadcast, domain_name, domain_name_servers, domain_search, host_name, interface_mtu\n"
            "noarp\n"
            "timeout {}\n",
            DhcpTimeout);

        THROW_LAST_ERROR_IF(WriteToFile(DHCPCD_CONF_PATH, Config.c_str()) < 0);

        execl(DHCPCD_PATH, DHCPCD_PATH, "-w", "-4", "-f", DHCPCD_CONF_PATH, "eth0", NULL);
        LOG_ERROR("execl({}) failed, {}", DHCPCD_PATH, errno);
    });

    if (ChildPid < 0)
    {
        return -1;
    }

    return WaitForChild(ChildPid, DHCPCD_PATH);
}

int StartGuestNetworkService(int GnsFd, wil::unique_fd&& DnsTunnelingFd, uint32_t DnsTunnelingIpAddress)

/*++

Routine Description:

    Start the guest network service.

Arguments:

    GnsFd - Supplies the socket file descriptor to use for the guest network service.

    DnsTunnelingFd - Supplies an optional file descriptor to be used for DNS tunneling.

    DnsTunnelingIpAddress - IP address to be used by the DNS tunneling listener.

Return Value:

    0 on success, -1 on failure.

--*/

{
    const auto ChildPid =
        UtilCreateChildProcess("GuestNetworkService", [GnsFd, DnsTunnelingFd = std::move(DnsTunnelingFd), DnsTunnelingIpAddress]() {
            std::string GnsSocketArg = std::to_string(GnsFd);
            THROW_LAST_ERROR_IF(SetCloseOnExec(GnsFd, false) < 0);

            if (DnsTunnelingFd)
            {
                std::string DnsSocketArg = std::to_string(DnsTunnelingFd.get());
                THROW_LAST_ERROR_IF(SetCloseOnExec(DnsTunnelingFd.get(), false) < 0);

                in_addr address{.s_addr = DnsTunnelingIpAddress};
                Address dnsIp = Address::FromBinary(AF_INET, 32, &address);
                execl(
                    LX_INIT_PATH,
                    LX_INIT_GNS,
                    LX_INIT_GNS_SOCKET_ARG,
                    GnsSocketArg.c_str(),
                    LX_INIT_GNS_DNS_SOCKET_ARG,
                    DnsSocketArg.c_str(),
                    LX_INIT_GNS_DNS_TUNNELING_IP,
                    dnsIp.Addr().c_str(),
                    nullptr);
            }
            else
            {
                execl(LX_INIT_PATH, LX_INIT_GNS, LX_INIT_GNS_SOCKET_ARG, GnsSocketArg.c_str(), nullptr);
            }

            LOG_ERROR("execl failed, {}", errno);
        });

    return (ChildPid < 0) ? -1 : 0;
}

void StartPortTracker(LX_MINI_INIT_PORT_TRACKER_TYPE Type)

/*++

Routine Description:

    Start a port tracker daemon.

Arguments:

    Type - specifies the type of port tracker (localhost relay or mirrored).

Return Value:

    None.

--*/

{
    auto PortTrackerFd = UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, false);
    if (!PortTrackerFd)
    {
        return;
    }

    wil::unique_fd NetlinkSocket{};
    wil::unique_fd BpfFd{};
    wil::unique_fd GuestRelayFd{};
    switch (Type)
    {
    case LxMiniInitPortTrackerTypeMirrored:
    {

        //
        // Create a netlink socket before registering the bpf filter so creation of the socket
        // does not trigger the filter.
        //

        NetlinkSocket = CreateNetlinkSocket();
        if (!NetlinkSocket)
        {
            return;
        }

        BpfFd = RegisterSeccompHook();
        if (!BpfFd)
        {
            return;
        }

        break;
    }
    case LxMiniInitPortTrackerTypeRelay:
    {
        sockaddr_vm HvSocketAddress = {};
        GuestRelayFd.reset(UtilListenVsockAnyPort(&HvSocketAddress, -1, false));
        if (!GuestRelayFd)
        {
            return;
        }

        break;
    }
    default:
        assert(false);
        return;
    }

    UtilCreateChildProcess(
        "PortTracker",
        [PortTrackerFd = std::move(PortTrackerFd),
         NetlinkSocket = std::move(NetlinkSocket),
         BpfFd = std::move(BpfFd),
         GuestRelayFd = std::move(GuestRelayFd)]() {
            execl(
                LX_INIT_PATH,
                LX_INIT_LOCALHOST_RELAY,
                INIT_PORT_TRACKER_FD_ARG,
                std::format("{}", PortTrackerFd.get()).c_str(),
                INIT_BPF_FD_ARG,
                std::format("{}", BpfFd.get()).c_str(),
                INIT_NETLINK_FD_ARG,
                std::format("{}", NetlinkSocket.get()).c_str(),
                INIT_PORT_TRACKER_LOCALHOST_RELAY,
                std::format("{}", GuestRelayFd.get()).c_str(),
                NULL);

            LOG_ERROR("execl failed {}", errno);
        });
}

int Initialize(const char* Hostname)

/*++

Routine Description:

    This routine performs initialization required for mini_init functionality.

Arguments:

    Hostname - Supplies a string specifying the hostname.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    //
    // Allow unprivileged users to view the kernel log.
    //

    if (WriteToFile(PROCFS_PATH "/sys/kernel/dmesg_restrict", "0\n") < 0)
    {
        return -1;
    }

    //
    // Set max inotify watches to the value suggested by Visual Studio Code Remote.
    //

    if (WriteToFile(PROCFS_PATH "/sys/fs/inotify/max_user_watches", "524288\n") < 0)
    {
        return -1;
    }

    //
    // Increase the soft and hard limit for number of open file descriptors.
    // N.B. the soft limit shouldn't be too high. See https://github.com/microsoft/WSL/issues/12985 .
    //

    rlimit Limit{};
    Limit.rlim_cur = 1024 * 10;
    Limit.rlim_max = 1024 * 1024;
    if (setrlimit(RLIMIT_NOFILE, &Limit) < 0)
    {
        LOG_ERROR("setrlimit(RLIMIT_NOFILE) failed {}", errno);
        return -1;
    }

    //
    // Increase the maximum number of bytes of memory that may be locked into RAM.
    //

    Limit.rlim_cur = 0x4000000;
    Limit.rlim_max = 0x4000000;
    if (setrlimit(RLIMIT_MEMLOCK, &Limit) < 0)
    {
        LOG_ERROR("setrlimit(RLIMIT_MEMLOCK) failed {}", errno);
        return -1;
    }

    //
    // Enable the loopback interface.
    //

    wil::unique_fd Fd{socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)};
    if (!Fd)
    {
        LOG_ERROR("socket failed {}", errno);
        return -1;
    }

    if (EnableInterface(Fd.get(), "lo") < 0)
    {
        return -1;
    }

    //
    // Enable logging when processes receive fatal signals.
    //

    if (WriteToFile("/proc/sys/kernel/print-fatal-signals", "1\n") < 0)
    {
        return -1;
    }

    //
    // Disable rate limiting of user writes to dmesg.
    //

    if (WriteToFile("/proc/sys/kernel/printk_devkmsg", "on\n") < 0)
    {
        return -1;
    }

    //
    // Set the hostname.
    //

    if (sethostname(Hostname, strlen(Hostname)) < 0)
    {
        LOG_ERROR("sethostname({}) failed {}", Hostname, errno);
    }

    //
    // Create a tmpfs mount for the cross-distro shared mount.
    //

    if (UtilMount(nullptr, CROSS_DISTRO_SHARE_PATH, "tmpfs", 0, nullptr) < 0)
    {
        return -1;
    }

    if (mount(nullptr, CROSS_DISTRO_SHARE_PATH, nullptr, MS_SHARED, nullptr) < 0)
    {
        LOG_ERROR("mount({}, MS_SHARED) failed {}", CROSS_DISTRO_SHARE_PATH, errno);
        return -1;
    }

    //
    // Create the resolv.conf symlink in the cross-distro share (gns writes to /etc/resolv.conf).
    //

    remove(RESOLV_CONF_PATH);
    if (symlink(CROSS_DISTRO_SHARE_PATH "/" RESOLV_CONF_FILE, RESOLV_CONF_PATH) < 0)
    {
        LOG_ERROR("symlink({}, {}) failed {}", CROSS_DISTRO_SHARE_PATH "/" RESOLV_CONF_FILE, RESOLV_CONF_PATH, errno);
        return -1;
    }

    //
    // Mount the binfmt_misc filesystem.
    //

    if (UtilMount(nullptr, BINFMT_PATH, "binfmt_misc", MS_RELATIME, nullptr) < 0)
    {
        return -1;
    }

    //
    // Register the Windows interop interpreter using the 'F' flag which makes
    // it available in other mount namespaces and chroot environments.
    //

    if (WriteToFile(BINFMT_PATH "/register", BINFMT_REGISTER_STRING) < 0)
    {
        return -1;
    }

    return 0;
}

int InitializeLogging(bool SetStderr, wil::LogFunction* ExceptionCallback) noexcept

/*++

Routine Description:

    This routine opens /dev/kmsg for logging and optionally sets it as stderr.

Arguments:

    SetStderr - Supplies a boolean specifying if kmsg should be set as stderr.

    ExceptionCallback - Supplies an optional callback to log exceptions.
        If not callback is specified, the default callback is used.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    wil::g_LogExceptionCallback = ExceptionCallback ? ExceptionCallback : LogException;
    auto devicePath = DEVFS_PATH "/kmsg";
    g_LogFd = TEMP_FAILURE_RETRY(open(devicePath, (O_WRONLY | O_CLOEXEC)));
    if (g_LogFd < 0)
    {
        g_LogFd = STDERR_FILENO;
        LOG_ERROR("open({}) failed {}", devicePath, errno);
        return -1;
    }
    else if (SetStderr)
    {
        if (g_LogFd != STDERR_FILENO)
        {
            if (dup2(g_LogFd, STDERR_FILENO) < 0)
            {
                LOG_ERROR("dup2({}, {}) failed {}", g_LogFd, STDERR_FILENO, errno);
                return -1;
            }

            close(g_LogFd);
            g_LogFd = STDERR_FILENO;
        }

        if (SetCloseOnExec(g_LogFd, false) < 0)
        {
            return -1;
        }
    }

    // Initialize logging to the hvc console device responsible for logging telemetry.
    if (UtilIsUtilityVm())
    {
        devicePath = DEVFS_PATH "/" LX_INIT_HVC_TELEMETRY;
        g_TelemetryFd = TEMP_FAILURE_RETRY(open(devicePath, (O_WRONLY | O_CLOEXEC)));
        if (g_TelemetryFd < 0)
        {
            LOG_ERROR("open({}) failed {}", devicePath, errno);
        }
    }

    return 0;
}

void InjectEntropy(gsl::span<gsl::byte> EntropyBuffer)

/*++

Routine Description:

    This routine injects boot-time entropy from the provided source.

Arguments:

    EntropyBuffer - Supplies a buffer of bytes to use as entropy.

Return Value:

    None.

--*/

{
    wil::unique_fd Fd{open(DEVFS_PATH "/random", O_RDWR)};
    if (!Fd)
    {
        LOG_ERROR("open failed {}", errno);
        return;
    }

    std::vector<gsl::byte> Buffer(sizeof(rand_pool_info) + EntropyBuffer.size());
    auto* PoolInfo = gslhelpers::get_struct<rand_pool_info>(gsl::make_span(Buffer));
    PoolInfo->entropy_count = EntropyBuffer.size() * 8;
    PoolInfo->buf_size = EntropyBuffer.size();
    gsl::copy(EntropyBuffer, gsl::as_writable_bytes(gsl::make_span(PoolInfo->buf, PoolInfo->buf_size)));
    if (ioctl(Fd.get(), RNDADDENTROPY, PoolInfo) < 0)
    {
        LOG_ERROR("ioctl(RNDADDENTROPY) failed {}", errno);
    }

    return;
}

void LaunchInit(
    int SocketFd,
    const char* Target,
    bool EnableGuiApps,
    const VmConfiguration& Config,
    const char* VmId,
    const char* DistributionName,
    const char* SharedMemoryRoot,
    const char* InstallPath,
    const char* UserProfile,
    std::optional<pid_t> DistroInitPid)

/*++

Routine Description:

    This routine launches the init daemon for the specified distro.

Arguments:

    SocketFd - Supplies a file descriptor to communicate with the init daemon.
        This routine takes ownership of this file descriptor.

    Target - Supplies the location where the distro filesystem is mounted.

    EnableGuiApps - True if GUI apps should be enabled.

    VmConfiguration - Supplies the VM configuration.

    VmId - Supplies the GUID of the VM. If this value is a non-empty string it
        is passed to init as an environment variable.

    DistributionName - Supplies the name of the distribution. If this value is a
        non-empty string it is passed to init as an environment variable.

    SharedMemoryRoot - Supplies the Windows OB path for virtiofs shared memory.
        If this value is a non-empty string, it is passed to init as an
        environment variable.

    InstallPath - Supplies the Windows path for the location where the lifted
        WSL package is installed. If this value is a non-empty string, it is
        passed to init as an environment variable.

    UserProfile - Supplies the Windows path for user profile of the VM owner.
        If this value is a non-empty string, it is passed to init as an
        environment variable.

    DistroInitPid - Supplies the pid of the user distribution's init process.

Return Value:

    None. This method does not return.

--*/

try
{
    std::vector<std::string> Variables;
    auto AddEnvironmentVariable = [&Variables](const char* Name, const char* Value) {
        if ((Value) && (*Value != '\0'))
        {
            Variables.emplace_back(std::format("{}={}", Name, Value));
        }
    };

    size_t TargetPathLength = strlen(Target);
    auto AddTemporaryMount = [&](const char* Name, const char* Source, unsigned long MountFlags) {
        std::string Path;
        THROW_LAST_ERROR_IF(CreateTempDirectory(Target, Path) < 0);
        THROW_LAST_ERROR_IF(mount(Source, Path.c_str(), nullptr, MountFlags, nullptr) < 0);
        AddEnvironmentVariable(Name, Path.substr(TargetPathLength).data());
    };

    //
    // Set the communication channel to expected file descriptor value.
    //

    if (SocketFd != LX_INIT_UTILITY_VM_INIT_SOCKET_FD)
    {
        THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(SocketFd, LX_INIT_UTILITY_VM_INIT_SOCKET_FD)) < 0);

        close(SocketFd);
        SocketFd = LX_INIT_UTILITY_VM_INIT_SOCKET_FD;
    }
    else
    {

        //
        // Remove the CLOEXEC flag since this fd is to be passed down to init.
        //

        THROW_LAST_ERROR_IF(SetCloseOnExec(SocketFd, false));
    }

    //
    // Move the cross-distro shared mount to a temporary location. This mount
    // will be moved by the distro init.
    //

    bool readOnly = false;
    try
    {
        AddTemporaryMount(LX_WSL2_CROSS_DISTRO_ENV, CROSS_DISTRO_SHARE_PATH, (MS_MOVE | MS_REC));
    }
    catch (...)
    {
        //
        // Creating the temporary mount can fail if:
        // - The distro VHD was mounted read-only (because a fsck is needed)
        // - The distro VHD is full
        //
        // Mount a writable overlay if that's the case so the distro can start.
        //

        LOG_WARNING("Detected read-only or full filesystem. Adding a tmpfs overlay");

        const std::string tmpfsTarget = std::format("{}-rw", Target);
        THROW_LAST_ERROR_IF(UtilMkdir(Target, 0755) < 0);

        THROW_LAST_ERROR_IF(UtilMountOverlayFs(tmpfsTarget.c_str(), Target) < 0);
        THROW_LAST_ERROR_IF(mount(tmpfsTarget.c_str(), Target, NULL, MS_BIND, NULL) < 0);

        AddTemporaryMount(LX_WSL2_CROSS_DISTRO_ENV, CROSS_DISTRO_SHARE_PATH, (MS_MOVE | MS_REC));
        readOnly = true;
        AddEnvironmentVariable(LX_WSL2_DISTRO_READ_ONLY_ENV, "1");
    }

    //
    // If GUI support is enabled, move the WSLg shared mount to a temporary
    // location. This mount will be moved by the distro init.
    //

    if (EnableGuiApps)
    {
        AddTemporaryMount(LX_WSL2_SYSTEM_DISTRO_SHARE_ENV, WSLG_PATH, (MS_MOVE | MS_REC));
    }

    //
    // Add other environment variables.
    //

    //
    // Init needs to know its pid relative to the root pid namespace.
    // Since the root namespace /proc is still mounted, it can be recovered by /proc/self.
    //

    auto pid = std::filesystem::read_symlink(PROCFS_PATH "/self");

    AddEnvironmentVariable(LX_WSL_PID_ENV, pid.c_str());
    AddEnvironmentVariable(LX_WSL2_VM_ID_ENV, VmId);
    AddEnvironmentVariable(LX_WSL2_DISTRO_NAME_ENV, DistributionName);
    AddEnvironmentVariable(LX_WSL2_SHARED_MEMORY_OB_DIRECTORY, SharedMemoryRoot);
    AddEnvironmentVariable(LX_WSL2_INSTALL_PATH, InstallPath);
    AddEnvironmentVariable(LX_WSL2_USER_PROFILE, UserProfile);
    AddEnvironmentVariable(LX_WSL2_NETWORKING_MODE_ENV, std::to_string(static_cast<int>(Config.NetworkingMode)).c_str());

    if (DistroInitPid.has_value())
    {
        AddEnvironmentVariable(LX_WSL2_DISTRO_INIT_PID, std::to_string(static_cast<int>(DistroInitPid.value())).c_str());
    }

    if (Config.EnableSafeMode)
    {
        AddEnvironmentVariable(LX_WSL2_SAFE_MODE, c_trueString);
    }

    //
    // If GPU support is enabled, move the GPU share mounts to temporary
    // mount points inside the distro. These will be moved by the distro init
    // process, or unmounted if GPU support is disabled via /etc/wsl.conf.
    //

    if (Config.EnableGpuSupport)
    {
        std::string Lower = GPU_SHARE_LIB_PACKAGED;
        if (Config.EnableInboxGpuLibs)
        {
            Lower += std::format(":{}", GPU_SHARE_LIB_INBOX);
        }

        THROW_LAST_ERROR_IF(UtilMountOverlayFs(GPU_SHARE_LIB, Lower.c_str(), (MS_NOATIME | MS_NOSUID | MS_NODEV), c_defaultRetryTimeout) < 0);

        for (int ShareIndex = 0; ShareIndex < COUNT_OF(g_gpuShares); ShareIndex += 1)
        {
            auto SharePath = std::format("{}{}", GPU_SHARE_PREFIX, g_gpuShares[ShareIndex].Name);
            auto ShareVariable = std::format("{}{}", LX_WSL2_GPU_SHARE_ENV, g_gpuShares[ShareIndex].Name);
            AddTemporaryMount(ShareVariable.c_str(), SharePath.c_str(), MS_MOVE);
        }
    }

    //
    // If kernel modules are supported, move the mount to a temporary location.
    // This mount will be moved by the distro init.
    //

    if (!Config.KernelModulesPath.empty())
    {
        AddTemporaryMount(LX_WSL2_KERNEL_MODULES_MOUNT_ENV, Config.KernelModulesPath.c_str(), (MS_MOVE | MS_REC));
        AddEnvironmentVariable(LX_WSL2_KERNEL_MODULES_PATH_ENV, Config.KernelModulesPath.c_str());
    }

    //
    // Bind mount the init daemon into the distro namespace.
    //

    auto Path = std::format("{}{}", Target, LX_INIT_PATH);
    THROW_LAST_ERROR_IF(MountInit(Path.c_str()) < 0);

    if (readOnly)
    {
        //
        // If a rw overlay was added, mark it as read-only.
        //

        THROW_LAST_ERROR_IF(mount(nullptr, Target, nullptr, MS_REMOUNT | MS_RDONLY, nullptr) < 0);
    }

    //
    // Change the root of the calling process to the distro mountpoint.
    //

    THROW_LAST_ERROR_IF(Chroot(Target) < 0);

    //
    // Exec the init daemon.
    //

    std::vector<char*> Environment;
    for (auto& e : Variables)
    {
        Environment.emplace_back(e.data());
    }

    assert(Environment.size() == Variables.size());

    Environment.push_back(nullptr);

    execle(LX_INIT_PATH, LX_INIT_PATH, nullptr, Environment.data());
    LOG_ERROR("execle({}) failed {}", LX_INIT_PATH, errno);
    _exit(1);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    _exit(1);
}

void LaunchSystemDistro(
    int SocketFd,
    const char* Target,
    const VmConfiguration& Config,
    const char* VmId,
    const char* DistributionName,
    const char* SharedMemoryRoot,
    const char* InstallPath,
    const char* UserProfile,
    pid_t DistroInitPid)

/*++

Routine Description:

    This routine launches the system distro.

Arguments:

    SocketFd - Supplies a file descriptor to communicate with the init daemon.
        This routine takes ownership of this file descriptor.

    Target - Supplies the location where the distro filesystem is mounted.

    VmConfiguration - Supplies the VM configuration.

    VmId - Supplies the GUID of the VM. If this value is a non-empty string it
        is passed to init as an environment variable.

    DistributionName - Supplies the name of the distribution. If this value is a
        non-empty string it is passed to init as an environment variable.

    SharedMemoryRoot - Supplies the Windows OB path for virtiofs shared memory.
        If this value is a non-empty string, it is passed to init as an
        environment variable.

    InstallPath - Supplies the Windows path for the location where the lifted
        WSL package is installed. If this value is a non-empty string, it is
        passed to init as an environment variable.

    UserProfile - Supplies the Windows path for user profile of the VM owner.
        If this value is a non-empty string, it is passed to init as an
        environment variable.

    DistroInitPid - Supplies the pid of the user distribution's init process.

Return Value:

    None. This method does not return.

--*/

try
{
    //
    // Create a writable layer on top of the read-only vhd.
    //

    THROW_LAST_ERROR_IF(UtilMountOverlayFs(Target, SYSTEM_DISTRO_VHD_PATH) < 0);

    //
    // Launch the init daemon, this method does not return.
    //

    LaunchInit(SocketFd, Target, true, Config, VmId, DistributionName, SharedMemoryRoot, InstallPath, UserProfile, DistroInitPid);
    _exit(1);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    _exit(1);
}

std::set<pid_t> ListInitChildProcesses()
{
    std::set<pid_t> children;

    auto content = wsl::shared::string::ReadFile<char>("/proc/self/task/1/children");

    for (const auto& e : wsl::shared::string::Split<char>(content, ' '))
    {
        children.insert(std::stoul(e, nullptr, 10));
    }

    return children;
}

std::vector<unsigned int> ListScsiDisks()
{
    std::vector<unsigned int> disks;

    for (const auto& e : std::filesystem::directory_iterator(SCSI_DEVICE_PATH))
    {
        auto filename = e.path().filename().string();
        if (filename.find(SCSI_DEVICE_NAME_PREFIX) == 0)
        {
            try
            {
                disks.emplace_back(std::stoul(filename.substr(strlen(SCSI_DEVICE_NAME_PREFIX))));
            }
            CATCH_LOG();
        }
    }

    return disks;
}

void LogException(const char* Message, const char* Description) noexcept

/*++

Routine Description:

    Callback to log exception information.

Arguments:

    Message - Supplies the message to log.

    Description - Supplies the exception description.

Return Value:

    None.

--*/

{
    if (Message)
    {
        dprintf(g_LogFd, "<3>WSL (%d) ERROR: %s %s", getpid(), Message, Description);
    }
    else
    {
        dprintf(g_LogFd, "<3>WSL (%d) ERROR: %s", getpid(), Description);
    }
}

int MountDevice(LX_MINI_INIT_MOUNT_DEVICE_TYPE DeviceType, unsigned int DeviceId, const char* Target, const char* FsType, unsigned int Flags, const char* Options)

/*++

Routine Description:

    This routine mounts the specified device.

Arguments:

    DeviceType - Supplies the type of device to mount.

    DeviceId - Supplies identifier for the SCSI or pmem device to mount.

    Target - Supplies the target of the mount.

    FsType - Supplies the filesystem type.

    Flags - Supplies flags for the operation.

    Options - Supplies mount options.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    //
    // Build the /dev path of the device.
    //

    std::string DevicePath;
    switch (DeviceType)
    {
    case LxMiniInitMountDeviceTypeLun:
        DevicePath = GetLunDevicePath(DeviceId);
        break;

    case LxMiniInitMountDeviceTypePmem:
        DevicePath = std::format("{}/pmem{}", DEVFS_PATH, DeviceId);
        break;

    default:
        LOG_ERROR("Unexpected DeviceType {}", DeviceType);
        return -EINVAL;
    }

    //
    // Mount to a temporary location if overlayfs was requested; otherwise, mount
    // the device directly on the target.
    //

    std::string MountPoint;
    if (Flags & LxMiniInitMessageFlagCreateOverlayFs)
    {
        if (CreateTempDirectory(Target, MountPoint) < 0)
        {
            return -1;
        }
    }
    else
    {
        MountPoint += Target;
    }

    //
    // Perform the mount.
    //

    const unsigned long MountFlags = (Flags & LxMiniInitMessageFlagMountReadOnly) ? MS_RDONLY : 0;
    if (UtilMount(DevicePath.c_str(), MountPoint.c_str(), FsType, MountFlags, Options, c_defaultRetryTimeout) < 0)
    {
        return -1;
    }

    //
    // Create an overlayfs mount for a read/write layer if requested.
    //

    if (Flags & LxMiniInitMessageFlagCreateOverlayFs)
    {
        if (UtilMountOverlayFs(Target, MountPoint.c_str()) < 0)
        {
            return -1;
        }
    }

    return 0;
}
CATCH_RETURN_ERRNO()

int MountPlan9(const char* Name, const char* Target, bool ReadOnly, std::optional<int> BufferSize)

/*++

Routine Description:

    This routine will mount a 9p share.

Arguments:

    Name - Supplies the aname of the 9p share to mount.

    Target - Supplies the mount target.

    ReadOnly - Supplies a boolean specifying if the share should be mounted as read-only.

    BufferSize - Optionally supplies a buffer size to use for the hvsocket send / receive buffers and 9p msize.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    int Size = BufferSize.value_or(LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE);
    wil::unique_fd Fd{UtilConnectVsock(LX_INIT_UTILITY_VM_PLAN9_PORT, true, Size)};
    if (!Fd)
    {
        return -1;
    }

    unsigned long Flags = MS_NOATIME | MS_NOSUID | MS_NODEV;
    auto Options = std::format("msize={},trans=fd,rfdno={},wfdno={},cache=mmap,aname={}", Size, Fd.get(), Fd.get(), Name);
    if (ReadOnly)
    {
        WI_SetFlag(Flags, MS_RDONLY);
        Options += ";fmask=222;dmask=222";
    }

    return UtilMount(Name, Target, PLAN9_FS_TYPE, Flags, Options.c_str(), c_defaultRetryTimeout);
}
CATCH_RETURN_ERRNO()

int MountSystemDistro(LX_MINI_INIT_MOUNT_DEVICE_TYPE DeviceType, unsigned int DeviceId)

/*++

Routine Description:

    This routine mounts the system distro as read-only, creates a writable
    tmpfs layer using overlayfs, and chroots to the mount point.

Arguments:

    DeviceType - Supplies the type of device to mount.

    DeviceId - Supplies identifier for the SCSI or pmem device to mount.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    //
    // Mount the system distro device as read-only.
    //

    const unsigned int Flags = LxMiniInitMessageFlagMountReadOnly;
    auto* Options = (DeviceType == LxMiniInitMountDeviceTypePmem) ? "dax" : nullptr;
    if (MountDevice(DeviceType, DeviceId, SYSTEM_DISTRO_VHD_PATH, "ext4", Flags, Options) < 0)
    {
        return -1;
    }

    //
    // Create a read / write overlay layer.
    //

    if (UtilMountOverlayFs(SYSTEM_DISTRO_PATH, SYSTEM_DISTRO_VHD_PATH) < 0)
    {
        return -1;
    }

    //
    // Move the devtmpfs, procfs, sysfs and system distro vhd mounts before chrooting.
    //

    for (const auto* Source : {DEVFS_PATH, PROCFS_PATH, SYSFS_PATH, SYSTEM_DISTRO_VHD_PATH})
    {
        auto Target = std::format("{}{}", SYSTEM_DISTRO_PATH, Source);
        if (UtilMount(Source, Target.c_str(), nullptr, (MS_MOVE | MS_REC), nullptr) < 0)
        {
            return -1;
        }
    }

    //
    // Create a bind mount of WSL init.
    //

    if (MountInit(SYSTEM_DISTRO_PATH LX_INIT_PATH) < 0)
    {
        return -1;
    }

    //
    // Chroot to system distro mount point.
    //
    // N.B. This allows running binaries present in the system distro without having to chroot.
    //

    return Chroot(SYSTEM_DISTRO_PATH);
}

std::map<unsigned long, std::string> ListDiskPartitions(const std::string& DeviceName, std::optional<unsigned long> SearchForIndex)

/*++

Routine Description:

    This routine returns the list of partitions in a block device.

Arguments:

    DeviceName - Supplies the block device name.

    SearchForIndex - Supplies a partition index to search for.

Return Value:

    A map from partition index to device name.

--*/

{
    std::string DevicePath = std::format("/sys/block/{}", DeviceName);

    return wsl::shared::retry::RetryWithTimeout<std::map<unsigned long, std::string>>(
        [&]() {
            wil::unique_dir Dir{opendir(DevicePath.c_str())};
            THROW_LAST_ERROR_IF(!Dir);

            std::map<unsigned long, std::string> partitions;

            for (auto Entry = readdir64(Dir.get()); Entry != nullptr; Entry = readdir64(Dir.get()))
            {
                if ((Entry->d_type != DT_DIR) || (strstr(Entry->d_name, DeviceName.c_str()) != Entry->d_name))
                {
                    continue;
                }

                partitions.emplace(GetDiskPartitionIndex(DevicePath.c_str(), Entry->d_name), Entry->d_name);
            }

            THROW_ERRNO_IF(ENOENT, SearchForIndex.has_value() && partitions.find(SearchForIndex.value()) == partitions.end());

            return partitions;
        },
        c_defaultRetryPeriod,
        c_defaultRetryTimeout,
        []() {
            auto err = wil::ResultFromCaughtException();
            return err == ENOENT || err == ENXIO;
        });
}

int MountDiskPartition(const char* DevicePath, const char* Type, const char* Target, unsigned long Flags, const char* Options, size_t PartitionIndex, PLX_MINI_MOUNT_STEP Step)

/*++

Routine Description:

    Mount a disk partition with a timeout.

Arguments:

    DevicePath - Path of the device to mount.

    Type - The filesystem to use.

    Target - The mount target.

    Flags - The mount flags.

    Options - The mount options.

    PartitionIndex - The partition to mount.

    Step - Pointer to update the current mount step.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    *Step = LxMiniInitMountStepFindPartition;
    if (!wsl::shared::string::StartsWith(DevicePath, DEVFS_PATH "/"))
    {
        LOG_ERROR("unexpected device path {}", DevicePath);
        return -1;
    }

    auto* DeviceName = &DevicePath[sizeof(DEVFS_PATH)];

    //
    // Find the partition on the specified device.
    //
    // N.B. A retry is needed because there is a delay between when a device is
    //      hot-added, and when the device is available in the guest.
    //

    auto partitions = ListDiskPartitions(DeviceName, PartitionIndex);

    auto partition = partitions.find(PartitionIndex);

    THROW_ERRNO_IF(ENOENT, partition == partitions.end());

    std::string partitionPath = std::format("/dev/{}", partition->second);
    LOG_INFO("Mapped partition {} from device {} to {}", PartitionIndex, DeviceName, partitionPath);

    //
    // Detect the filesystem type.
    //

    *Step = LxMiniInitMountStepDetectFilesystem;
    std::string DetectedFilesystem;
    if (Type == nullptr)
    {
        if (DetectFilesystem(partitionPath.c_str(), DetectedFilesystem) < 0)
        {
            return -1;
        }

        Type = DetectedFilesystem.c_str();
    }

    *Step = LxMiniInitMountStepMount;
    return UtilMount(partitionPath.c_str(), Target, Type, Flags, Options, c_defaultRetryTimeout);
}
CATCH_RETURN()

int MountInit(const char* Target)

/*++

Routine Description:

    This routine create a read-only bind mount of the init daemon at the specified target.

Arguments:

    Target - Supplies the target for the mount.

Return Value:

    0 on success, < 0 on failure.

--*/

try
{
    wil::unique_fd InitFd{open(Target, (O_CREAT | O_WRONLY | O_TRUNC), 0755)};
    THROW_LAST_ERROR_IF(!InitFd);

    THROW_LAST_ERROR_IF(mount(LX_INIT_PATH, Target, nullptr, (MS_RDONLY | MS_BIND), nullptr) < 0);

    THROW_LAST_ERROR_IF(mount(nullptr, Target, nullptr, (MS_RDONLY | MS_REMOUNT | MS_BIND), nullptr) < 0);

    return 0;
}
CATCH_RETURN_ERRNO()

std::string GetMountTarget(const char* Name)

/*++

Routine Description:

    Generate the path to a mount target.

Arguments:

    Name - Supplies the mount name.

    Target - The buffer receiving the mountpoint target.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    return std::format("{}/{}", CROSS_DISTRO_SHARE_PATH, Name);
}

void ProcessLaunchInitMessage(
    const LX_MINI_INIT_MESSAGE* Message,
    gsl::span<gsl::byte> Buffer,
    wsl::shared::SocketChannel&& Channel,
    wil::unique_fd&& SystemDistroSocketFd,
    const VmConfiguration& Config)
{
    //
    // Send a message back to the service that contains the pid of the child process.
    // If the distribution terminates unexpectedly, this pid will be sent to the service so it knows that the instance
    // has terminated.
    //

    LX_MINI_CREATE_INSTANCE_STEP Step = LxInitCreateInstanceStepMountDisk;

    auto ReportStatus = [&Channel, &Step](auto result) {
        LX_MINI_INIT_CREATE_INSTANCE_RESULT message{};
        message.Header.MessageType = LxMiniInitMessageCreateInstanceResult;
        message.Header.MessageSize = sizeof(message);
        message.FailureStep = Step;
        message.Result = result;

        Channel.SendMessage(message);
    };

    try
    {
        auto* FsType = wsl::shared::string::FromSpan(Buffer, Message->FsTypeOffset);
        auto* MountOptions = wsl::shared::string::FromSpan(Buffer, Message->MountOptionsOffset);

        //
        // Mount the device.
        //

        THROW_LAST_ERROR_IF(MountDevice(Message->MountDeviceType, Message->DeviceId, DISTRO_PATH, FsType, Message->Flags, MountOptions) < 0);

        //
        // Allow /etc/wsl.conf in the user distro to opt-out of GUI support.
        //
        // N.B. A connection for the system distro must established even if the distro opts out
        //      of GUI app support because WslService is waiting to accept a connection.
        //

        bool enableGuiApps = Config.EnableGuiApps;
        if (Message->Flags & LxMiniInitMessageFlagLaunchSystemDistro && Config.EnableGuiApps)
        {
            Step = LxInitCreateInstanceStepLaunchSystemDistro;
            wil::unique_file File{fopen(DISTRO_PATH ETC_PATH "/wsl.conf", "r")};
            if (File)
            {
                std::vector<ConfigKey> ConfigKeys = {ConfigKey("general.guiApplications", enableGuiApps)};
                ParseConfigFile(ConfigKeys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(CONFIG_FILE));
                File.reset();
            }

            //
            // If the distro did not opt-out of GUI applications, continue launching the system distro.
            //

            if (enableGuiApps)
            {
                //
                // Create a tmpfs mount for a shared folder between user and system distro.
                //

                THROW_LAST_ERROR_IF(UtilMount(nullptr, WSLG_PATH, "tmpfs", 0, nullptr) < 0);

                THROW_LAST_ERROR_IF(mount(nullptr, WSLG_PATH, nullptr, MS_SHARED, nullptr) < 0);

                //
                // Create a directory to store x11 sockets.
                //
                // N.B. This needs to be created early so a bind mount into the shared WSLg location
                //      can be created on top of the hard-coded location expected by x11 clients.
                //

                THROW_LAST_ERROR_IF(UtilMkdir(WSLG_PATH "/" X11_SOCKET_NAME, 0777) < 0);

                //
                // Create a read-only bind mount of the user distro into the shared WSLg folder so fonts and icons can be accessed.
                //

                THROW_LAST_ERROR_IF(UtilMount(DISTRO_PATH, WSLG_PATH DISTRO_PATH, nullptr, (MS_BIND | MS_RDONLY), nullptr) < 0);

                THROW_LAST_ERROR_IF(UtilMount(nullptr, WSLG_PATH DISTRO_PATH, nullptr, (MS_RDONLY | MS_REMOUNT | MS_BIND), nullptr) < 0);

                //
                // Create a child process in a new mount, pid, and UTS namespace (with a shared IPC namespace).
                // This child process will become the user distro init daemon.
                //

                auto ChildPid = CLONE(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD);
                THROW_LAST_ERROR_IF(ChildPid < 0);

                if (ChildPid > 0)
                {
                    //
                    // Close the socket for the user distro and launch the system
                    // distro. This method does not return.
                    //

                    Channel.Close();

                    LaunchSystemDistro(
                        SystemDistroSocketFd.get(),
                        SYSTEM_DISTRO_PATH,
                        Config,
                        wsl::shared::string::FromSpan(Buffer, Message->VmIdOffset),
                        wsl::shared::string::FromSpan(Buffer, Message->DistributionNameOffset),
                        wsl::shared::string::FromSpan(Buffer, Message->SharedMemoryRootOffset),
                        wsl::shared::string::FromSpan(Buffer, Message->InstallPathOffset),
                        wsl::shared::string::FromSpan(Buffer, Message->UserProfileOffset),
                        ChildPid);
                }
            }

            SystemDistroSocketFd.reset();
        }

        //
        // Launch the distro init daemon, this method does not return.
        //

        Step = LxInitCreateInstanceStepLaunchInit;
        LaunchInit(
            Channel.Socket(),
            DISTRO_PATH,
            enableGuiApps,
            Config,
            wsl::shared::string::FromSpan(Buffer, Message->VmIdOffset),
            wsl::shared::string::FromSpan(Buffer, Message->DistributionNameOffset),
            nullptr,
            wsl::shared::string::FromSpan(Buffer, Message->InstallPathOffset),
            wsl::shared::string::FromSpan(Buffer, Message->UserProfileOffset));
    }
    catch (...)
    {
        ReportStatus(wil::ResultFromCaughtException());
        _exit(1);
    }
}

void PostProcessImportedDistribution(wsl::shared::MessageWriter<LX_MINI_INIT_IMPORT_RESULT>& Message, const char* ExtractedPath)
{
    //
    // Save the current working directory as a file descriptor so it can be restored.
    //

    wil::unique_fd cwdFd{open(".", O_RDONLY | O_DIRECTORY)};
    THROW_LAST_ERROR_IF(!cwdFd);

    auto restoreCwd = wil::scope_exit([&cwdFd]() {
        THROW_LAST_ERROR_IF(fchdir(cwdFd.get()) < 0);
        THROW_LAST_ERROR_IF(chroot(".") < 0);
    });

    //
    // Chroot to the extracted path to validate distro contents.
    //
    // N.B. The chroot is needed because the distro may contain absolute symlinks (for example, /bin/sh may symlink to /bin/toolbox).
    //

    THROW_LAST_ERROR_IF(chdir(ExtractedPath) < 0);
    THROW_LAST_ERROR_IF(chroot(".") < 0);

    Message->ValidDistribution = false;

    for (auto* path : {"/etc", "/bin/sh"})
    {
        if (access(path, F_OK) >= 0)
        {
            Message->ValidDistribution = true;
        }
    }

    if (!Message->ValidDistribution)
    {
        return;
    }

    auto [flavor, version] = UtilReadFlavorAndVersion("/etc/os-release");

    if (flavor.has_value())
    {
        Message.WriteString(Message->FlavorIndex, flavor.value());
    }

    if (version.has_value())
    {
        Message.WriteString(Message->VersionIndex, version.value());
    }

    std::string defaultName{};
    std::string shortcutIconPath;
    std::string terminalProfileTemplatePath;
    Message->GenerateTerminalProfile = true;
    Message->GenerateShortcut = true;

    std::vector<ConfigKey> keys = {
        ConfigKey("shortcut.icon", shortcutIconPath),
        ConfigKey("shortcut.enabled", Message->GenerateShortcut),
        ConfigKey("oobe.defaultName", defaultName),
        ConfigKey("windowsterminal.profileTemplate", terminalProfileTemplatePath),
        ConfigKey("windowsterminal.enabled", Message->GenerateTerminalProfile)};

    {
        wil::unique_file File{fopen(WSL_DISTRIBUTION_CONF, "r")};
        ParseConfigFile(keys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(WSL_DISTRIBUTION_CONF));
    }

    if (!defaultName.empty())
    {
        Message.WriteString(Message->DefaultNameIndex, defaultName);
    }

    try
    {
        if (!shortcutIconPath.empty())
        {
            // Prevent escaping the distribution install path.
            if (shortcutIconPath.find("..") != std::string::npos)
            {
                LOG_ERROR("Invalid format for shortcut.icon: {}", shortcutIconPath.c_str());
                THROW_ERRNO(EINVAL);
            }

            auto iconBuffer = UtilReadFileRaw(shortcutIconPath.c_str(), 1024 * 1024);
            gsl::copy(
                gsl::as_writable_bytes(gsl::make_span(iconBuffer)),
                Message.InsertBuffer(Message->ShortcutIconIndex, iconBuffer.size(), Message->ShortcutIconSize));
        }
    }
    CATCH_LOG();

    try
    {
        if (Message->GenerateTerminalProfile && !terminalProfileTemplatePath.empty())
        {
            // Prevent escaping the distribution install path.
            if (terminalProfileTemplatePath.find("..") != std::string::npos)
            {
                LOG_ERROR("Invalid format for windows-terminal.profile_template: {}", terminalProfileTemplatePath.c_str());
                THROW_ERRNO(EINVAL);
            }

            auto content = UtilReadFileRaw(terminalProfileTemplatePath.c_str(), 1024 * 1024);
            gsl::copy(
                gsl::as_writable_bytes(gsl::make_span(content)),
                Message.InsertBuffer(Message->TerminalProfileIndex, content.size(), Message->TerminalProfileSize));
        }
    }
    CATCH_LOG();
}

void ProcessImportExportMessage(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel&& Channel)
{
    const LX_MINI_INIT_MESSAGE* Message{};
    sockaddr_vm ListenAddress{};
    wil::unique_fd ListenSocket;
    int Result = -1;

    {
        auto ReportStatus = wil::scope_exit([&Channel, &Result, &ListenAddress]() {
            LX_MINI_INIT_CREATE_INSTANCE_RESULT message{};
            message.Header.MessageType = LxMiniInitMessageCreateInstanceResult;
            message.Header.MessageSize = sizeof(message);
            message.FailureStep = LxInitCreateInstanceStepMountDisk;
            message.Result = Result;
            message.ConnectPort = ListenAddress.svm_port;
            Channel.SendMessage(message);
        });

        try
        {
            Message = gslhelpers::try_get_struct<LX_MINI_INIT_MESSAGE>(Buffer);
            THROW_ERRNO_IF(EINVAL, !Message);

            ListenSocket = UtilListenVsockAnyPort(&ListenAddress, 2, true);
            THROW_LAST_ERROR_IF(!ListenSocket);

            if (Message->Header.MessageType == LxMiniInitMessageImport)
            {
                THROW_LAST_ERROR_IF(FormatDevice(Message->DeviceId) < 0);
            }

            auto* FsType = wsl::shared::string::FromSpan(Buffer, Message->FsTypeOffset);
            auto* MountOptions = wsl::shared::string::FromSpan(Buffer, Message->MountOptionsOffset);
            THROW_LAST_ERROR_IF(MountDevice(Message->MountDeviceType, Message->DeviceId, DISTRO_PATH, FsType, Message->Flags, MountOptions) < 0);

            Result = 0;
        }
        catch (...)
        {
            Result = wil::ResultFromCaughtException();
        }
    }

    if (Result < 0)
    {
        LOG_ERROR("ProcessImportExportMessage failed, {}", errno);
        return;
    }

    Result = -1;
    auto ReportStatus = wil::scope_exit([&Channel, &Result, MessageType = Message->Header.MessageType]() {
        if (MessageType == LxMiniInitMessageExport)
        {
            if (UtilWriteBuffer(Channel.Socket(), &Result, sizeof(Result)) < 0)
            {
                LOG_ERROR("response write failed {}", errno);
            }
        }
        else
        {
            wsl::shared::MessageWriter<LX_MINI_INIT_IMPORT_RESULT> message;
            message->Result = Result;
            if (Result == 0)
            {
                PostProcessImportedDistribution(message, DISTRO_PATH);
            }

            Channel.SendMessage<LX_MINI_INIT_IMPORT_RESULT>(message.Span());
        }
    });

    wil::unique_fd DataSocket{UtilAcceptVsock(ListenSocket.get(), ListenAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
    THROW_LAST_ERROR_IF(!DataSocket);

    wil::unique_fd ErrorSocket{UtilAcceptVsock(ListenSocket.get(), ListenAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)};
    THROW_LAST_ERROR_IF(!ErrorSocket);

    switch (Message->Header.MessageType)
    {
    case LxMiniInitMessageImport:
        Result = ImportFromSocket(DISTRO_PATH, DataSocket.get(), ErrorSocket.get(), Message->Flags);
        break;

    case LxMiniInitMessageExport:
        Result = ExportToSocket(DISTRO_PATH, DataSocket.get(), ErrorSocket.get(), Message->Flags);
        break;

    case LxMiniInitMessageImportInplace:
        Result = 0;
        break;

    default:
        LOG_ERROR("Unexpected message type {}", Message->Header.MessageType);
    }
}

int ProcessMountFolderMessage(wsl::shared::SocketChannel& Channel, gsl::span<gsl::byte> Buffer)

/*++

Routine Description:

    Mount a filesystem as requested by the mount message

Arguments:

    Buffer - Supplies the mount message.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    auto* Message = gslhelpers::try_get_struct<LX_MINI_INIT_MOUNT_FOLDER_MESSAGE>(Buffer);
    if (!Message)
    {
        LOG_ERROR("Unexpected message size {}", Buffer.size());
        return -1;
    }

    const auto* Target = wsl::shared::string::FromSpan(Buffer, Message->PathIndex);
    const auto* Name = wsl::shared::string::FromSpan(Buffer, Message->NameIndex);

    if (Target == nullptr || Name == nullptr)
    {
        LOG_ERROR("Invalid name or path index in LX_MINI_INIT_MOUNT_FOLDER_MESSAGE");
        return -1;
    }

    int Result = MountPlan9(Name, Target, Message->ReadOnly);
    Channel.SendResultMessage<int32_t>(Result);
    return 0;
}

int ProcessMountMessage(gsl::span<gsl::byte> Buffer)

/*++

Routine Description:

    Mount a filesystem as requested by the mount message

Arguments:

    Buffer - Supplies the mount message.

Return Value:

    0 on success, < 0 on failure.

--*/

{
    wil::unique_fd SocketFd{UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true)};
    if (!SocketFd)
    {
        return -1;
    }

    const int ChildPid = UtilCreateChildProcess(
        "DiskMount", [Buffer, Channel = wsl::shared::SocketChannel{std::move(SocketFd), "MountResult"}]() mutable {
            // Set up a scope exit variable to report mount status.
            int Result = -1;
            LX_MINI_MOUNT_STEP Step = LxMiniInitMountStepFindDevice;
            auto ReportStatus = wil::scope_exit([&Channel, &Result, &Step]() { ReportMountStatus(Channel, Result, Step); });

            auto* Header = gslhelpers::try_get_struct<MESSAGE_HEADER>(Buffer);
            if (!Header)
            {
                LOG_ERROR("Unexpected message size {}", Buffer.size());
                return;
            }

            std::string Device;
            std::string DetectedFilesystem;
            std::string Target;
            if (Header->MessageType == LxMiniInitMessageMount)
            {
                auto* Message = gslhelpers::try_get_struct<LX_MINI_INIT_MOUNT_MESSAGE>(Buffer);
                if (!Message)
                {
                    LOG_ERROR("Unexpected message size {}", Buffer.size());
                    return;
                }

                Device = GetLunDevicePath(Message->ScsiLun);

                //
                // Construct the target of the mount.
                //

                Target = GetMountTarget(wsl::shared::string::FromSpan(Buffer, Message->TargetNameOffset));

                //
                // Determine the type of mount. If no type was specified, detect it with blkid.
                //

                const auto* Type = wsl::shared::string::FromSpan(Buffer, Message->TypeOffset);
                if (*Type == '\0')
                {
                    Type = nullptr;
                }

                //
                // Parse the mount flags.
                //

                auto* MountOptions = wsl::shared::string::FromSpan(Buffer, Message->OptionsOffset);
                auto ParsedOptions = mountutil::MountParseFlags(MountOptions == nullptr ? "" : MountOptions);

                //
                // Perform the mount.
                //

                if (Message->PartitionIndex == 0)
                {
                    Step = LxMiniInitMountStepDetectFilesystem;
                    if (Type == nullptr)
                    {
                        Result = DetectFilesystem(Device.c_str(), DetectedFilesystem);
                        if (Result < 0)
                        {
                            return;
                        }

                        Type = DetectedFilesystem.c_str();
                    }

                    Step = LxMiniInitMountStepMount;
                    Result = UtilMount(
                        Device.c_str(), Target.c_str(), Type, ParsedOptions.MountFlags, ParsedOptions.StringOptions.c_str(), c_defaultRetryTimeout);
                }
                else
                {
                    Result = MountDiskPartition(
                        Device.c_str(),
                        Type,
                        Target.c_str(),
                        ParsedOptions.MountFlags,
                        ParsedOptions.StringOptions.c_str(),
                        Message->PartitionIndex,
                        &Step);
                }
            }
            else if (Header->MessageType == LxMiniInitMessageUnmount)
            {
                auto* Message = gslhelpers::try_get_struct<LX_MINI_INIT_UNMOUNT_MESSAGE>(Buffer);
                if (!Message)
                {
                    LOG_ERROR("Unexpected message size {}", Buffer.size());
                    return;
                }

                Target = GetMountTarget(Message->Buffer);

                Step = LxMiniInitMountStepUnmount;
                Result = umount(Target.c_str());
                if (Result < 0)
                {
                    Result = -errno;
                    LOG_ERROR("umount({}) failed, {}", Target.c_str(), errno);
                    return;
                }

                Step = LxMiniInitMountStepRmDir;
                Result = rmdir(Target.c_str());
                if (Result < 0)
                {
                    Result = -errno;
                    LOG_ERROR("rmdir({}) failed, {}", Target.c_str(), errno);
                }
            }
            else
            {
                assert(Header->MessageType == LxMiniInitMessageDetach);
                auto* Message = gslhelpers::try_get_struct<LX_MINI_INIT_DETACH_MESSAGE>(Buffer);
                if (!Message)
                {
                    LOG_ERROR("Unexpected message size {}", Buffer.size());
                    return;
                }

                Result = DetachScsiDisk(Message->ScsiLun);
            }
        });

    return (ChildPid < 0) ? -1 : 0;
}

int ReportMountStatus(wsl::shared::SocketChannel& Channel, int Result, LX_MINI_MOUNT_STEP Step)

/*++

Routine Description:

    Report the result of a mount / unmount operation to via an hvsocket.

Arguments:

    Channel - Supplies the socket channel.

    Result - Supplies the operation result code.

    Step - Supplies the step at which the mount operation failed, if any.

Return Value:

    0 on success, < 0 on failure.

--*/
try
{
    LX_MINI_INIT_MOUNT_RESULT_MESSAGE Message;
    Message.Header.MessageSize = sizeof(Message);
    Message.Header.MessageType = LxMiniInitMessageMountStatus;
    Message.Result = Result;
    Message.FailureStep = Step;

    Channel.SendMessage(Message);

    return 0;
}
CATCH_RETURN_ERRNO();

int ProcessWaitForPmemDeviceMessage(PLX_MINI_INIT_WAIT_FOR_PMEM_DEVICE_MESSAGE Message)

/*++

Routine Description:

    This routine processes a message that waits for a pmem device to appear under /dev.
    The actual waiting is performed asynchronously.

Arguments:

    Message - The wait for pmem device message

Return Value:

    0 on success, < 0 on failure.

--*/

{
    wsl::shared::SocketChannel Channel{wil::unique_fd{UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true)}, "WaitForPmem"};
    if (Channel.Socket() < 0)
    {
        return -1;
    }

    const int ChildPid = UtilCreateChildProcess("PMemDeviceWait", [&Channel, PmemId = Message->PmemId]() {
        int Result = -1;
        auto ReportStatus = wil::scope_exit([&Channel, &Result]() { Channel.SendResultMessage<int32_t>(Result); });

        //
        // Construct the device path.
        //

        std::string DevicePath = std::format("{}/pmem{}", DEVFS_PATH, PmemId);

        //
        // Poll for the device to appear. Ideally we'd replace this with something
        // like libudev so we can be notified when devices appear.
        //

        struct stat Buffer;
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_LAST_ERROR_IF(stat(DevicePath.c_str(), &Buffer) < 0); },
            c_defaultRetryPeriod,
            c_defaultRetryTimeout,
            [&]() {
                Result = -wil::ResultFromCaughtException();
                return Result == -ENOENT;
            });

        Result = 0;
    });

    if (ChildPid < 0)
    {
        Channel.SendResultMessage<int32_t>(errno);
        return -1;
    }

    return 0;
}

int ProcessResizeDistributionMessage(gsl::span<gsl::byte> Buffer)
try
{
    auto* Message = gslhelpers::try_get_struct<LX_MINI_INIT_RESIZE_DISTRIBUTION_MESSAGE>(Buffer);

    if (!Message)
    {
        LOG_ERROR("Unexpected message size {}", Buffer.size());
        return -1;
    }

    wil::unique_fd SocketFd{UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true)};
    if (!SocketFd)
    {
        return -1;
    }

    wil::unique_fd OutputSocketFd{UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true)};
    if (!OutputSocketFd)
    {
        return -1;
    }

    const int ChildPid = UtilCreateChildProcess(
        "ResizeDistribution",
        [Message, Channel = wsl::shared::SocketChannel{std::move(SocketFd), "ResizeDistribution"}, OutputSocket = std::move(OutputSocketFd)]() mutable {
            int ResponseCode = -1;
            auto ReportStatus = wil::scope_exit([&]() {
                LX_MINI_INIT_RESIZE_DISTRIBUTION_RESPONSE ResponseMessage{};
                ResponseMessage.ResponseCode = ResponseCode;
                ResponseMessage.Header.MessageType = LxMiniInitMessageResizeDistributionResponse;
                ResponseMessage.Header.MessageSize = sizeof(ResponseMessage);

                Channel.SendMessage(ResponseMessage);
            });

            THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(OutputSocket.get(), STDOUT_FILENO)) < 0);
            THROW_LAST_ERROR_IF(TEMP_FAILURE_RETRY(dup2(OutputSocket.get(), STDERR_FILENO)) < 0);

            auto DevicePath = GetLunDevicePath(Message->ScsiLun);

            auto CommandLine = std::format("/usr/sbin/e2fsck -f -y '{}'", DevicePath);
            THROW_LAST_ERROR_IF(UtilExecCommandLine(CommandLine.c_str()) < 0);

            if (Message->NewSize == 0)
            {
                CommandLine = std::format("/usr/sbin/resize2fs '{}'", DevicePath);
            }
            else
            {
                CommandLine = std::format("/usr/sbin/resize2fs '{}' '{}K'", DevicePath, ((Message->NewSize + 1024) - 1) / 1024);
            }

            THROW_LAST_ERROR_IF(UtilExecCommandLine(CommandLine.c_str()) < 0);

            ResponseCode = 0;
        });

    return (ChildPid < 0) ? -1 : 0;
}
CATCH_RETURN_ERRNO();

int ProcessMessage(wsl::shared::SocketChannel& Channel, LX_MESSAGE_TYPE Type, gsl::span<gsl::byte> Buffer, VmConfiguration& Config)

/*++

Routine Description:

    This routine processes messages from the service.

Arguments:

    MessageFd - Supplies a file descriptor to the socket on which the message was
        received. This is used for operations that require responses, for example a
        VHD eject request.

    Buffer - Supplies the message.

    Config - Supplies the VM configuration.

Return Value:

    0 on success, -1 on failure.

--*/
try
{

    //
    // Validate the message and handle operations that do not require creating a child process.
    //

    switch (Type)
    {
    case LxMiniInitMessageLaunchInit:
    case LxMiniInitMessageImport:
    case LxMiniInitMessageImportInplace:
    case LxMiniInitMessageExport:
        try
        {
            const auto Message = gslhelpers::try_get_struct<LX_MINI_INIT_MESSAGE>(Buffer);
            THROW_ERRNO_IF(EINVAL, !Message);

            wsl::shared::SocketChannel Channel{UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, false), "Init"};
            if (Channel.Socket() < 0)
            {
                return -1;
            }

            wil::unique_fd SystemDistroSocketFd{};
            if (Message->Flags & LxMiniInitMessageFlagLaunchSystemDistro && Config.EnableGuiApps)
            {
                SystemDistroSocketFd = UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, false);
                if (!SystemDistroSocketFd)
                {
                    return -1;
                }
            }

            auto ChildPid = UtilCreateChildProcess(
                "LaunchDistro",
                [Type, Message, Buffer, Channel = std::move(Channel), SystemDistroSocketFd = std::move(SystemDistroSocketFd), &Config]() mutable {
                    //
                    // Restore the default signal flags so anything blocked by mini_init doesn't get
                    // inherited by init and session leaders.
                    //

                    THROW_LAST_ERROR_IF(UtilRestoreBlockedSignals() < 0);

                    if (Type == LxMiniInitMessageLaunchInit)
                    {
                        ProcessLaunchInitMessage(Message, Buffer, std::move(Channel), std::move(SystemDistroSocketFd), Config);
                        FATAL_ERROR("Unexpected return from ProcessLaunchInitMessage");
                    }
                    else
                    {
                        ProcessImportExportMessage(Buffer, std::move(Channel));
                    }
                },
                (CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD));

            return (ChildPid < 0) ? -1 : 0;
        }
        CATCH_RETURN_ERRNO()

    case LxMiniInitMessageEjectVhd:
    {
        //
        // Eject the scsi device and inform the service that the operation is complete.
        //

        const auto* EjectMessage = gslhelpers::try_get_struct<EJECT_VHD_MESSAGE>(Buffer);
        if (!EjectMessage)
        {
            LOG_ERROR("Unexpected message size {}", Buffer.size());
            return -1;
        }

        Channel.SendResultMessage(EjectScsi(EjectMessage->Lun));
        return 0;
    }

    case LxMiniInitMessageEarlyConfig:
    {
        const auto EarlyConfig = gslhelpers::try_get_struct<LX_MINI_INIT_EARLY_CONFIG_MESSAGE>(Buffer);
        if (!EarlyConfig)
        {
            LOG_ERROR("Unexpected message size {}", Buffer.size());
            return -1;
        }

        if (EarlyConfig->EnableSafeMode)
        {
            LOG_WARNING("{} - many features will be disabled", WSL_SAFE_MODE_WARNING);
            Config.EnableSafeMode = true;
        }

        //
        // Establish the connection for the guest network service.
        //

        auto SocketFd = UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true);
        if (!SocketFd)
        {
            return -1;
        }

        //
        // If DNS tunneling is enabled, open a separate hvsocket connection for it.
        //

        wil::unique_fd DnsTunnelingSocketFd{};
        if (EarlyConfig->EnableDnsTunneling)
        {
            DnsTunnelingSocketFd = UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true);
            if (!DnsTunnelingSocketFd)
            {
                return -1;
            }
        }

        //
        // Configure page reporting and memory reclamation.
        //

        ConfigureMemoryReduction(EarlyConfig->PageReportingOrder, EarlyConfig->MemoryReclaimMode);

        //
        // Initialize system distro if supported.
        //

        if (EarlyConfig->SystemDistroDeviceId != UINT_MAX)
        {
            if (MountSystemDistro(EarlyConfig->SystemDistroDeviceType, EarlyConfig->SystemDistroDeviceId) < 0)
            {
                return -1;
            }

            //
            // Crash dump collection needs to be reconfigured here, because we called chroot.
            //

            if (Config.EnableCrashDumpCollection)
            {
                EnableCrashDumpCollection();
            }

            Config.EnableSystemDistro = true;

            //
            // Set the $LANG environment variable.
            //
            // N.B. This is needed by bsdtar for path conversions (to support .xz file format).
            //

            if (setenv("LANG", "en_US.UTF-8", 1) < 0)
            {
                LOG_ERROR("setenv(LANG, en_US.UTF-8) failed {}", errno);
            }

            //
            // Start the debug shell if enabled.
            //

            if (EarlyConfig->EnableDebugShell)
            {
                StartDebugShell();
            }

            //
            // Configure swap space.
            //

            if (EarlyConfig->SwapLun != UINT_MAX)
            {
                CreateSwap(EarlyConfig->SwapLun);
            }

            //
            // Start the time sync agent (chronyd) to keep guest clock in sync with the host.
            //

            StartTimeSyncAgent();
        }

        //
        // Mount kernel modules if supported.
        //
        // N.B. The VHD is mounted as read-only but with a writable overlayfs layer. The modules
        //      directory must be writable for tools like depmod to work.
        //

        if (EarlyConfig->KernelModulesDeviceId != UINT_MAX)
        {
            THROW_LAST_ERROR_IF(
                MountDevice(LxMiniInitMountDeviceTypeLun, EarlyConfig->KernelModulesDeviceId, KERNEL_MODULES_VHD_PATH, "ext4", LxMiniInitMessageFlagMountReadOnly, nullptr) <
                0);

            utsname UnameBuffer{};
            THROW_LAST_ERROR_IF(uname(&UnameBuffer) < 0);

            std::string Target = std::format("{}/{}", KERNEL_MODULES_PATH, UnameBuffer.release);
            THROW_LAST_ERROR_IF(UtilMountOverlayFs(Target.c_str(), KERNEL_MODULES_VHD_PATH, (MS_NOATIME | MS_NOSUID | MS_NODEV)) < 0);

            const std::string KernelModulesList = wsl::shared::string::FromSpan(Buffer, EarlyConfig->KernelModulesListOffset);
            for (const auto& Module : wsl::shared::string::Split(KernelModulesList, ','))
            {
                const char* Argv[] = {MODPROBE_PATH, Module.c_str(), nullptr};
                int Status = -1;
                auto result = UtilCreateProcessAndWait(MODPROBE_PATH, Argv, &Status);
                if (result < 0)
                {
                    LOG_ERROR("Failed to load module '{}', {}", Module, Status);
                }
            }

            Config.KernelModulesPath = std::move(Target);
        }

        //
        // Initialization required by mini_init.
        //

        if (Initialize(wsl::shared::string::FromSpan(Buffer, EarlyConfig->HostnameOffset)) < 0)
        {
            return -1;
        }

        //
        // Start the guest network service.
        //

        if (StartGuestNetworkService(SocketFd.get(), std::move(DnsTunnelingSocketFd), EarlyConfig->DnsTunnelingIpAddress) < 0)
        {
            return -1;
        }

        return 0;
    }

    case LxMiniInitMessageInitialConfig:
    {
        const auto ConfigMessage = gslhelpers::try_get_struct<LX_MINI_INIT_CONFIG_MESSAGE>(Buffer);
        if (!ConfigMessage)
        {
            LOG_ERROR("Unexpected message size {}", Buffer.size());
            return -1;
        }

        auto NetworkingConfiguration = &ConfigMessage->NetworkingConfiguration;
        Config.NetworkingMode = NetworkingConfiguration->NetworkingMode;
        if (NetworkingConfiguration->PortTrackerType != LxMiniInitPortTrackerTypeNone)
        {
            StartPortTracker(NetworkingConfiguration->PortTrackerType);
        }

        if (NetworkingConfiguration->DisableIpv6)
        {
            WriteToFile("/proc/sys/net/ipv6/conf/all/disable_ipv6", c_trueString);
        }

        if (NetworkingConfiguration->EnableDhcpClient)
        {
            StartDhcpClient(NetworkingConfiguration->DhcpTimeout);
        }

        if (SetEphemeralPortRange(NetworkingConfiguration->EphemeralPortRangeStart, NetworkingConfiguration->EphemeralPortRangeEnd) < 0)
        {
            return -1;
        }

        if (ConfigMessage->EntropySize > 0)
        {
            InjectEntropy(Buffer.subspan(ConfigMessage->EntropyOffset, ConfigMessage->EntropySize));
        }

        if (ConfigMessage->MountGpuShares)
        {
            if (MountPlan9(LXSS_GPU_DRIVERS_SHARE, GPU_SHARE_DRIVERS, true) < 0)
            {
                return -1;
            }

            if (MountPlan9(LXSS_GPU_PACKAGED_LIB_SHARE, GPU_SHARE_LIB_PACKAGED, true) < 0)
            {
                return -1;
            }

            if (ConfigMessage->EnableInboxGpuLibs)
            {
                if (MountPlan9(LXSS_GPU_INBOX_LIB_SHARE, GPU_SHARE_LIB_INBOX, true) < 0)
                {
                    return -1;
                }
            }
        }

        Config.EnableInboxGpuLibs = ConfigMessage->EnableInboxGpuLibs;
        Config.EnableGpuSupport = ConfigMessage->MountGpuShares;
        Config.EnableGuiApps = ConfigMessage->EnableGuiApps;
        return 0;
    }
    case LxMiniInitMessageMount:
    case LxMiniInitMessageUnmount:
    case LxMiniInitMessageDetach:
        ProcessMountMessage(Buffer);

        //
        // Ignore the return code from ProcessMountMessage so that we don't exit on error.
        //

        return 0;

    case LxMiniInitMountFolder:
        return ProcessMountFolderMessage(Channel, Buffer);

    case LxInitCreateProcess:
        return ProcessCreateProcessMessage(Channel, Buffer);

    case LxMiniInitMessageWaitForPmemDevice:
    {
        const auto PmemMessage = gslhelpers::try_get_struct<LX_MINI_INIT_WAIT_FOR_PMEM_DEVICE_MESSAGE>(Buffer);
        if (!PmemMessage)
        {
            LOG_ERROR("Unexpected message size {}", Buffer.size());
            return -1;
        }

        ProcessWaitForPmemDeviceMessage(PmemMessage);

        //
        // Ignore the return code from ProcessWaitForPmemDeviceMessage so that we don't exit on error.
        //

        return 0;
    }

    case LxMiniInitMessageResizeDistribution:
    {

        ProcessResizeDistributionMessage(Buffer);
        return 0;
    }

    default:
        LOG_ERROR("Unexpected message type {}", Type);
        return -1;
    }

    _exit(1);
}
CATCH_RETURN_ERRNO();

wil::unique_fd RegisterSeccompHook()

/*++

Routine Description:

    Register a seccomp notification for bind() & ioctl(*, TUNSETIFF, *) calls.

Arguments:

    None.

Return Value:

    The notification file descriptor or < 0 on failure.

--*/

{
    struct sock_filter Filter[] = {
        // Structure of this program:
        // For each architecture, there is a block of instructions to match specific calls.
        // The first two instructions check for the arch and skip to the next one if it doesn't match.
        // Each block contains a return SECCOMP_RET_USER_NOTIF/SECCOMP_RET_ALLOW so that
        // offset within a block don't change as other blocks change.

        // 64bit:
        // If syscall_arch & __AUDIT_ARCH_64BIT then continue else goto :32bit
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_arch),
        // For now, notify on all non-native arch
        BPF_JUMP(BPF_JMP + BPF_JSET + BPF_K, __AUDIT_ARCH_64BIT, 0, 7),
        // If syscall_nr == __NR_bind then goto user_notify: else continue
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_nr),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_bind, 3, 0),
        // if (syscall_nr == __NR_bind) then continue else goto allow:
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_ioctl, 0, 3),
        // if (syscall arg1 == SIOCSIFFLAGS) goto user_notify else goto allow:
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_arg(1)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, SIOCSIFFLAGS, 0, 1),
        // user_notify:
        //     return SECCOMP_RET_USER_NOTIF;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_USER_NOTIF),
        // allow:
        //     return SECCOMP_RET_ALLOW;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

    // Note: 32bit on x86_64 uses the __NR_socketcall with the first argument
    // set to SYS_BIND to make bind system call.
#ifdef __x86_64__
        // 32bit:
        // If syscall_nr == __NR_socketcall then continue else goto allow:
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_nr),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, I386_NR_socketcall, 0, 3),
        // if syscall arg0 == SYS_BIND then goto user_notify: else goto allow:
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_arg(0)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, SYS_BIND, 0, 1),
        // user_notify:
        //     return SECCOMP_RET_USER_NOTIF;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_USER_NOTIF),
        // allow:
        //     return SECCOMP_RET_ALLOW;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
#else
        // 32bit:
        // If syscall_nr == __NR_bind then goto user_notify: else goto allow:
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, syscall_nr),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARMV7_NR_bind, 0, 1),
        // user_notify:
        //     return SECCOMP_RET_USER_NOTIF;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_USER_NOTIF),
        // allow:
        //     return SECCOMP_RET_ALLOW;
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
#endif
    };

    struct sock_fprog Prog = {
        .len = sizeof(Filter) / sizeof(Filter[0]),
        .filter = Filter,
    };

    wil::unique_fd Fd{syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &Prog)};
    if (!Fd)
    {
        LOG_ERROR("Failed to register bpf syscall hook, {}", errno);
        return {};
    }

    if (SetCloseOnExec(Fd.get(), false) < 0)
    {
        return {};
    }

    return Fd;
}

int SendCapabilities(wsl::shared::SocketChannel& Channel)

/*++

Routine Description:

    Send the kernel capabilities on the specified channel.

Arguments:

    Channel - The channel to send the message on.

Return Value:

    0 on success or < 0 on failure.

--*/

try
{
    utsname Version;
    THROW_LAST_ERROR_IF(uname(&Version) < 0);

    wsl::shared::MessageWriter<LX_INIT_GUEST_CAPABILITIES> Message(LxMiniInitMessageGuestCapabilities);
    Message.WriteString(Version.release);

    //
    // SECCOMP_USER_NOTIF_FLAG_CONTINUE is the latest flag that flow steering needs
    // but there's no way to test for its presence. The assumption is that if seccomp is available
    // and the kernel version is >= 5.10, then SECCOMP_USER_NOTIF_FLAG_CONTINUE is available
    //

    uint32_t SeccompFlag = SECCOMP_RET_USER_NOTIF;
    Message->SeccompAvailable = syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &SeccompFlag) == 0;

    Channel.SendMessage<LX_INIT_GUEST_CAPABILITIES>(Message.Span());
    return 0;
}
CATCH_RETURN_ERRNO();

int SetCloseOnExec(int Fd, bool Enable)

/*++

Routine Description:

    Sets or clears the FD_CLOEXEC flag on the file descriptor.

Arguments:

    Fd - Supplies the file descriptor to modify.

    Enable - true to set the flag, false to clear.

Return Value:

    0 on success or -1 on failure.

--*/

{
    int Result = fcntl(Fd, F_GETFD, 0);
    if (Result < 0)
    {
        LOG_ERROR("fcntl(F_GETFD) failed {}", errno);
        return -1;
    }

    int Flags = Enable ? (Result | FD_CLOEXEC) : (Result & ~FD_CLOEXEC);
    Result = fcntl(Fd, F_SETFD, Flags);
    if (Result < 0)
    {
        LOG_ERROR("fcntl(F_SETFD, {}) failed {}", Flags, errno);
        return -1;
    }

    return 0;
}

int SetEphemeralPortRange(uint16_t Start, uint16_t End)

/*++

Routine Description:

    This routine sets the ephemeral port range.

Arguments:

    Start - Supplies the first port of the range (inclusive)

    End - Supplies the last port of the range (inclusive).

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    if (Start == 0 && End == 0)
    {
        return 0;
    }

    std::string Content = std::format("{} {}", Start, End);

    //
    // N.B. IPv6 reads from /proc/sys/net/ipv4/ip_local_port_range as well according to
    //      https://tldp.org/HOWTO/Linux+IPv6-HOWTO/ch11s03.html.
    //

    return WriteToFile("/proc/sys/net/ipv4/ip_local_port_range", Content.c_str());
}
CATCH_RETURN_ERRNO()

void StartTimeSyncAgent()

/*++

Routine Description:

    This routine configures and launches chronyd.

Arguments:

    None.

Return Value:

    None.

--*/

{
    //
    // Check if the /dev/ptp0 device is present.
    //

    if (access("/dev/ptp0", F_OK) < 0)
    {
        LOG_ERROR("/dev/ptp0 not found - kernel must be built with CONFIG_PTP_1588_CLOCK");
        return;
    }

    //
    // Create a child process to run chronyd.
    //

    UtilCreateChildProcess("chrony", []() {
        const auto FileContents =
            "driftfile /var/lib/chrony/drift\n" // Record the rate at which the system clock gains/losses time.
            "makestep 1.0 3\n" // Allow the system clock to be stepped in the first three updates if its offset is larger than 1 second.
            "rtcsync\n"        // Enable kernel synchronization of the real-time clock (RTC).
            "leapsectz right/UTC\n"    // Get TAI-UTC offset and leap seconds from the system tz database.
            "logdir /var/log/chrony\n" // Specify directory for log files.
            "refclock PHC /dev/ptp0 poll 3 dpoll -2 offset 0\n"; // Use the /dev/ptp0 device as a clock source.

        remove(CHRONY_CONF_PATH);
        THROW_LAST_ERROR_IF(WriteToFile(CHRONY_CONF_PATH, FileContents) < 0);

        execl(CHRONYD_PATH, CHRONYD_PATH, NULL);
        LOG_ERROR("execl failed {}", errno);
    });
}

void WaitForBlockDevice(const char* Path)

/*++

Routine Description:

    Wait for a block device to be available.

Arguments:

    Path - Supplies the path to the block device.

Return Value:

    None.

--*/

{
    wsl::shared::retry::RetryWithTimeout<void>(
        [&]() {
            wil::unique_fd device{open(Path, O_RDONLY)};
            THROW_LAST_ERROR_IF(!device);
        },
        c_defaultRetryPeriod,
        c_defaultRetryTimeout,
        [&]() {
            errno = wil::ResultFromCaughtException();
            return errno == ENOENT || errno == ENXIO || errno == EIO;
        });
}

int WaitForChild(pid_t Pid, const char* Name)

/*++

Routine Description:

    Wait for a child process to exit and check that it exited successfully.

Arguments:

    Pid - Supplies the pid to wait for.

    Name - Supplies the process image name, for logging.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Status = -1;
    if (TEMP_FAILURE_RETRY(waitpid(Pid, &Status, 0)) < 0)
    {
        LOG_ERROR("Waiting for child '{}' failed, waitpid failed {}", Name, errno);
        return -1;
    }

    return UtilProcessChildExitCode(Status, Name);
}

int WslEntryPoint(int Argc, char* Argv[]);

void EnableDebugMode(const std::string& Mode)
{
    if (Mode == "hvsocket")
    {
        // Mount the debugfs.
        THROW_LAST_ERROR_IF(UtilMount("none", "/sys/kernel/debug", "debugfs", 0, nullptr) < 0);

        // Enable hvsocket events.
        std::vector<const char*> files{
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_on_msg_dpc/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_on_message/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_onoffer/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_onoffer_rescind/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_onopen_result/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_ongpadl_created/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_ongpadl_torndown/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_open/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_close_internal/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_establish_gpadl_header/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_establish_gpadl_body/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_teardown_gpadl/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_release_relid/enable",
            "/sys/kernel/debug/tracing/events/hyperv/vmbus_send_tl_connect_request/enable"};

        for (auto* e : files)
        {
            WriteToFile(e, "1");
        }

        // Relay logs to the host.
        std::thread relayThread{[]() {
            constexpr auto path = "/sys/kernel/debug/tracing/trace_pipe";
            std::ifstream file(path);

            if (!file)
            {
                LOG_ERROR("Failed to open {}, {}", path, errno);
                return;
            }

            std::string line;
            while (std::getline(file, line))
            {
                LOG_INFO("{}", line);
            }

            LOG_ERROR("{}: closed", path);
        }};

        relayThread.detach();
    }
    else
    {
        LOG_ERROR("Unknown debugging mode: '{}'", Mode);
    }
}

int main(int Argc, char* Argv[])
{
    std::vector<gsl::byte> Buffer;
    ssize_t BytesRead;
    VmConfiguration Config{};
    wil::unique_fd ConsoleFd{};
    wsl::shared::SocketChannel channel;
    wil::unique_fd NotifyFd{};
    struct pollfd PollDescriptors[2];
    wil::unique_fd SignalFd{};
    struct signalfd_siginfo SignalInfo;
    sigset_t SignalMask;
    int Status;

    //
    // Determine which entrypoint should be used.
    //

    if (getpid() != 1 || !getenv(WSL_ROOT_INIT_ENV))
    {
        return WslEntryPoint(Argc, Argv);
    }

    if (unsetenv(WSL_ROOT_INIT_ENV))
    {
        LOG_ERROR("unsetenv failed {}", errno);
    }

    // Use an env variable to determine whether socket logging is enabled since /proc isn't mounted yet
    // so SocketChannel can't look at the kernel command line.
    wsl::shared::SocketChannel::EnableSocketLogging(getenv(WSL_SOCKET_LOG_ENV) != nullptr);

    if (unsetenv(WSL_SOCKET_LOG_ENV))
    {
        LOG_ERROR("unsetenv failed {}", errno);
    }

    //
    // Mount devtmpfs.
    //

    int Result = UtilMount(nullptr, DEVFS_PATH, "devtmpfs", 0, nullptr);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    //
    // Open kmsg for logging and ensure that the file descriptor is not set to one of the standard file descriptors.
    //
    // N.B. This is to work around a rare race condition where init is launched without /dev/console set as the controlling terminal.
    //

    InitializeLogging(false);
    if (g_LogFd <= STDERR_FILENO)
    {
        LOG_ERROR("/init was started without /dev/console");
        if (dup2(g_LogFd, 3) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }

        close(g_LogFd);
        g_LogFd = 3;
    }

    //
    // Ensure /dev/console is present and set as the controlling terminal.
    // If opening /dev/console times out, stdout and stderr to the logging file descriptor.
    //

    try
    {
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                ConsoleFd = open("/dev/console", O_RDWR);
                THROW_LAST_ERROR_IF(!ConsoleFd);
            },
            c_defaultRetryPeriod,
            c_defaultRetryTimeout);

        THROW_LAST_ERROR_IF(login_tty(ConsoleFd.get()) < 0);
    }
    catch (...)
    {
        if (dup2(g_LogFd, STDOUT_FILENO) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }

        if (dup2(g_LogFd, STDERR_FILENO) < 0)
        {
            LOG_ERROR("dup2 failed {}", errno);
        }
    }

    //
    // Open /dev/null for stdin.
    //

    {
        wil::unique_fd Fd{TEMP_FAILURE_RETRY(open(DEVNULL_PATH, O_RDONLY))};
        if (!Fd)
        {
            LOG_ERROR("open({}) failed {}", DEVNULL_PATH, errno);
            return -1;
        }

        if (Fd.get() == STDIN_FILENO)
        {
            Fd.release();
        }
        else
        {
            if (TEMP_FAILURE_RETRY(dup2(Fd.get(), STDIN_FILENO)) < 0)
            {
                LOG_ERROR("dup2 failed {}", errno);
                return -1;
            }
        }
    }

    //
    // Create the etc directory and mount procfs and sysfs.
    //

    if (UtilMkdir(ETC_PATH, 0755) < 0)
    {
        return -1;
    }

    if (UtilMount(nullptr, PROCFS_PATH, "proc", 0, nullptr) < 0)
    {
        return -1;
    }

    if (UtilMount(nullptr, SYSFS_PATH, "sysfs", 0, nullptr) < 0)
    {
        return -1;
    }

    //
    // Enable debug mode, if specified.
    //

    if (const auto* debugMode = getenv(WSL_DEBUG_ENV))
    {
        LOG_ERROR("Running in debug mode: '{}'", debugMode);
        EnableDebugMode(debugMode);

        unsetenv(WSL_DEBUG_ENV);
    }

    //
    // Establish the message channel with the service via hvsocket.
    //

    channel = {UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true), "mini_init"};
    if (channel.Socket() < 0)
    {
        Result = -1;
        goto ErrorExit;
    }

    if (SendCapabilities(channel) < 0)
    {
        goto ErrorExit;
    }
    //
    // Create another channel for guest-driven communication, for example, to
    // notify the service when a distribution terminates unexpectedly.
    //

    NotifyFd = UtilConnectVsock(LX_INIT_UTILITY_VM_INIT_PORT, true);
    if (!NotifyFd)
    {
        Result = -1;
        goto ErrorExit;
    }

    if (getenv(WSL_ENABLE_CRASH_DUMP_ENV))
    {
        Config.EnableCrashDumpCollection = true;

        EnableCrashDumpCollection();
        if (unsetenv(WSL_ENABLE_CRASH_DUMP_ENV) < 0)
        {
            LOG_ERROR("unsetenv failed {}", errno);
        }
    }

    UtilMount(nullptr, CGROUP_MOUNTPOINT, CGROUP2_DEVICE, 0, nullptr);

    UtilSetThreadName("mini_init");

    //
    // Create a signalfd to detect when the child process exits.
    //

    sigemptyset(&SignalMask);
    sigaddset(&SignalMask, SIGCHLD);
    Result = UtilSaveBlockedSignals(SignalMask);
    if (Result < 0)
    {
        LOG_ERROR("sigprocmask failed {}", errno);
        goto ErrorExit;
    }

    SignalFd = signalfd(-1, &SignalMask, SFD_CLOEXEC);
    if (!SignalFd)
    {
        Result = -1;
        LOG_ERROR("signalfd failed {}", errno);
        goto ErrorExit;
    }

    //
    // Fill the poll descriptors and begin worker loop.
    //

    PollDescriptors[0].fd = channel.Socket();
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[1].fd = SignalFd.get();
    PollDescriptors[1].events = POLLIN;
    for (;;)
    {
        Result = poll(PollDescriptors, COUNT_OF(PollDescriptors), -1);
        if (Result < 0)
        {
            LOG_ERROR("poll failed {}", errno);
            break;
        }

        //
        // Process messages from the service. Break out of the loop if the socket is closed.
        //

        assert((PollDescriptors[0].revents & POLLNVAL) == 0);
        if (PollDescriptors[0].revents & (POLLHUP | POLLERR))
        {
            break;
        }
        else if (PollDescriptors[0].revents & POLLIN)
        {
            auto [Message, Range] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (Message == nullptr)
            {
                break; // Socket was closed, exit
            }

            Result = ProcessMessage(channel, Message->MessageType, Range, Config);
            if (Result < 0)
            {
                goto ErrorExit;
            }
        }

        //
        // Handle signalfd.
        //

        assert((PollDescriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) == 0);
        if (PollDescriptors[1].revents & POLLIN)
        {
            BytesRead = TEMP_FAILURE_RETRY(read(PollDescriptors[1].fd, &SignalInfo, sizeof(SignalInfo)));
            if (BytesRead != sizeof(SignalInfo))
            {
                Result = -1;
                LOG_ERROR("read failed {} {}", BytesRead, errno);
                goto ErrorExit;
            }

            if (SignalInfo.ssi_signo != SIGCHLD)
            {
                LOG_ERROR("Unexpected signal {}", SignalInfo.ssi_signo);
                goto ErrorExit;
            }

            //
            // Reap zombies and notify the service when child processes exit.
            //

            for (;;)
            {
                Result = waitpid(-1, &Status, WNOHANG);
                if (Result == 0)
                {
                    break;
                }
                else if (Result > 0)
                {
                    //
                    // Perform a sync to flush all writes.
                    //

                    sync();

                    //
                    // Send a message with the child's pid to the service.
                    //

                    LX_MINI_INIT_CHILD_EXIT_MESSAGE Message{};
                    Message.Header.MessageType = LxMiniInitMessageChildExit;
                    Message.Header.MessageSize = sizeof(Message);
                    Message.ChildPid = Result;
                    Result = UtilWriteBuffer(NotifyFd.get(), gslhelpers::struct_as_bytes(Message));
                    if (Result < 0)
                    {
                        LOG_ERROR("write failed {}", errno);
                    }
                }
                else
                {
                    //
                    // No more children exist.
                    //

                    if (errno != ECHILD)
                    {
                        LOG_ERROR("waitpid failed {}", errno);
                    }

                    break;
                }
            }
        }
    }

ErrorExit:
    try
    {
        auto children = ListInitChildProcesses();

        while (!children.empty())
        {

            // send SIGKILL to all running processes.
            for (auto pid : children)
            {
                if (kill(pid, SIGKILL) < 0)
                {
                    LOG_ERROR("Failed to send SIGKILL to {}: {}", pid, errno);
                }
            }

            // Wait for processes to actually exit.
            while (!children.empty())
            {
                auto Result = waitpid(-1, nullptr, 0);
                THROW_ERRNO_IF(errno, Result <= 0);
                LOG_INFO("Process {} exited", Result);
                children.erase(Result);
            }

            children = ListInitChildProcesses();
        }
    }
    CATCH_LOG();

    sync();

    try
    {
        for (auto disk : ListScsiDisks())
        {
            if (DetachScsiDisk(disk) < 0)
            {
                LOG_ERROR("Failed to detach disk: {}", disk);
            }
        }
    }
    CATCH_LOG();

    reboot(RB_POWER_OFF);

    return Result;
}
