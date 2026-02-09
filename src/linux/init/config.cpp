/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    config.c

Abstract:

    This file contains methods for configuring a running instance.

--*/

#include <bitset>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <pwd.h>
#include <future>
#include <signal.h>
#include <pty.h>
#include <lxbusapi.h>
#include "common.h"
#include "mountutilcpp.h"
#include "config.h"
#include "util.h"
#include "configfile.h"
#include "binfmt.h"
#include "wslpath.h"
#include "wslinfo.h"
#include "drvfs.h"
#include "timezone.h"
#include "message.h"
#include "WslDistributionConfig.h"
#include "lxfsshares.h"
#include "plan9.h"

#define AUTO_MOUNT_PARENT_MODE 0755
#define CGROUP_DEVICE "cgroup"
#define CGROUPS_FILE "/proc/cgroups"
#define CGROUPS_NO_V1 "cgroup_no_v1="
#define DEFAULT_CWD "/"
#define DRVFS_MOUNT_OPTIONS (MS_NOATIME)
#define DRVFS_SOURCE " :\\"
#define DRVFS_TARGET_MODE 0777
#define DRVFS_OPTIONS_BUFFER_LENGTH 38
#define ETC_DEFAULT_FOLDER ETC_FOLDER "default/"
#define HOSTNAME_FILE_PATH ETC_FOLDER "hostname"
#define HOSTNAME_FILE_MODE 0644
#define HOSTS_FILE_MODE 0644
#define HOSTS_FILE_PATH ETC_FOLDER "hosts"
#define LANG_ENV "LANG"
#define LOCALE_FILE_PATH ETC_DEFAULT_FOLDER "locale"
#define PATH_ENV "PATH"
#define RESOLV_CONF_DIRECTORY_MODE 0755
#define RESOLV_CONF_FILE_MODE 0644
#define RESOLV_CONF_FILE_NAME "resolv.conf"
#define RESOLV_CONF_FILE_PATH ETC_FOLDER RESOLV_CONF_FILE_NAME
#define RESOLV_CONF_FOLDER RUN_FOLDER "/resolvconf"
#define RESOLV_CONF_SYMLINK_TARGET ".." RESOLV_CONF_FOLDER "/" RESOLV_CONF_FILE_NAME
#define RESOLV_CONF_SYMLINK_WSL_MOUNT_SUFFIX SHARED_MOUNT_FOLDER "/" RESOLV_CONF_FILE_NAME
#define RUN_FOLDER "/run"
#define SHARED_MOUNT_FOLDER "wsl"
#define USER_MOUNT_FOLDER "user"
#define WINDOWS_LD_CONF_FILE "/etc/ld.so.conf.d/ld.wsl.conf"
#define WINDOWS_LD_CONF_FILE_MODE 0644

#define MOUNTS_FILE "/proc/self/mounts"
#define MOUNTS_FIELD_SEPARATOR ' '
#define MOUNTS_LINE_SEPARATOR '\n'
#define MOUNTS_DEVICE_FIELD 0
#define MOUNTS_FSTYPE_FIELD 2

using wsl::linux::WslDistributionConfig;

static void ConfigApplyWindowsLibPath(const wsl::linux::WslDistributionConfig& Config);

static bool CreateLoginSession(const wsl::linux::WslDistributionConfig& Config, const char* Username, uid_t Uid);

class RemoveMountAndEnvironmentOnScopeExit
{
public:
    RemoveMountAndEnvironmentOnScopeExit() = default;

    RemoveMountAndEnvironmentOnScopeExit(const char* EnvironmentName) : m_environmentName(EnvironmentName)
    {
        m_mountPath = getenv(m_environmentName);
    }

    RemoveMountAndEnvironmentOnScopeExit& operator=(const RemoveMountAndEnvironmentOnScopeExit&) = delete;
    RemoveMountAndEnvironmentOnScopeExit(const RemoveMountAndEnvironmentOnScopeExit&) = delete;

    RemoveMountAndEnvironmentOnScopeExit(RemoveMountAndEnvironmentOnScopeExit&& Other)
    {
        *this = std::move(Other);
    }

    RemoveMountAndEnvironmentOnScopeExit& operator=(RemoveMountAndEnvironmentOnScopeExit&& Other)
    {
        m_environmentName = Other.m_environmentName;
        Other.m_environmentName = nullptr;

        m_mountPath = Other.m_mountPath;
        Other.m_mountPath = nullptr;

        return *this;
    }

    ~RemoveMountAndEnvironmentOnScopeExit()
    {
        if (m_environmentName != nullptr)
        {
            if (unsetenv(m_environmentName) < 0)
            {
                LOG_ERROR("unsetenv({}) failed {}", m_environmentName, errno);
            }
        }

        if (m_mountPath != nullptr)
        {
            if (umount2(m_mountPath, MNT_DETACH) < 0)
            {
                LOG_ERROR("umount2({}, MNT_DETACH) failed {}", m_mountPath, errno);
                return;
            }

            if (rmdir(m_mountPath) < 0)
            {
                LOG_ERROR("rmdir({}) failed {}", m_mountPath, errno);
            }
        }
    }

    operator bool() const
    {
        return m_mountPath;
    }

    const char* MountPath() const
    {
        return m_mountPath;
    }

    bool MoveMount(const char* Target)
    {
        if (m_mountPath == nullptr)
        {
            return false;
        }

        if (UtilMount(m_mountPath, Target, nullptr, (MS_MOVE | MS_REC), nullptr) < 0)
        {
            return false;
        }

        if (rmdir(m_mountPath) < 0)
        {
            LOG_ERROR("rmdir({}) failed {}", m_mountPath, errno);
        }

        m_mountPath = nullptr;
        return true;
    }

private:
    const char* m_environmentName = nullptr;
    const char* m_mountPath = nullptr;
};

constexpr auto HostsFileFormatString = LX_INIT_AUTO_GENERATED_FILE_HEADER
    "# [network]\n"
    "# generateHosts = false\n"
    "127.0.0.1\tlocalhost\n"
    "127.0.1.1\t{}.{}\t{}\n"
    "{}\n"
    "# The following lines are desirable for IPv6 capable hosts\n"
    "::1     ip6-localhost ip6-loopback\n"
    "fe00::0 ip6-localnet\n"
    "ff00::0 ip6-mcastprefix\n"
    "ff02::1 ip6-allnodes\n"
    "ff02::2 ip6-allrouters\n";

constexpr auto WindowsLibSearchFileHeaderString = LX_INIT_AUTO_GENERATED_FILE_HEADER
    "# [automount]\n"
    "# ldconfig = false\n";

const INIT_STARTUP_ANY LxssStartupCommon[] = {
    INIT_ANY_DIRECTORY("/sys", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_DEVICE("/sys", "sysfs", "sysfs", (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_SHARED)),
    INIT_ANY_DIRECTORY("/proc", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_DEVICE("/proc", "proc", "proc", (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_SHARED)),
    INIT_ANY_DIRECTORY("/dev/block", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_SYMLINK("/dev/fd", "/proc/self/fd"),
    INIT_ANY_SYMLINK("/dev/stdin", "/proc/self/fd/0"),
    INIT_ANY_SYMLINK("/dev/stdout", "/proc/self/fd/1"),
    INIT_ANY_SYMLINK("/dev/stderr", "/proc/self/fd/2"),
    INIT_ANY_DIRECTORY("/dev/pts", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_DEVICE_OPTION("/dev/pts", "devpts", "devpts", "gid=5,mode=620", MS_NOATIME | MS_NOSUID | MS_NOEXEC),
    INIT_ANY_DIRECTORY("/run", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_OPTION("/run", "tmpfs", "mode=755", (MS_NODEV | MS_STRICTATIME | MS_NOSUID | MS_SHARED)),
    INIT_ANY_DIRECTORY("/run/lock", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT("/run/lock", "tmpfs", MS_NOATIME | MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_SHARED),
    INIT_ANY_DIRECTORY("/run/shm", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT("/run/shm", "tmpfs", MS_NOATIME | MS_NOSUID | MS_NODEV | MS_SHARED),
    INIT_ANY_DIRECTORY("/dev/shm", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_DEVICE("/dev/shm", nullptr, "/run/shm", MS_BIND),
    INIT_ANY_DIRECTORY("/run/user", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_OPTION("/run/user", "tmpfs", "mode=755", MS_NOATIME | MS_NOSUID | MS_NOEXEC | MS_NODEV),
    INIT_ANY_DIRECTORY("/bin", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_SYMLINK("/bin/" WSLINFO_NAME, "/init"),
    INIT_ANY_SYMLINK("/bin/" WSLPATH_NAME, "/init"),
    INIT_ANY_DIRECTORY("/sbin", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_SYMLINK("/sbin/" MOUNT_DRVFS_NAME, "/init"),
    INIT_ANY_MOUNT_DEVICE(BINFMT_MISC_MOUNT_TARGET, "binfmt_misc", "binfmt_misc", MS_RELATIME),
    INIT_ANY_DIRECTORY("/tmp", ROOT_UID, ROOT_GID, S_IFDIR | S_ISVTX | 0777)};

const INIT_STARTUP_ANY LxssStartupLoggingVmMode[] = {
    INIT_ANY_DIRECTORY("/dev", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_OPTION("/dev", "devtmpfs", "mode=755", (MS_NOSUID | MS_RELATIME | MS_SHARED))};

const INIT_STARTUP_ANY LxssStartupLoggingWsl[] = {
    INIT_ANY_DIRECTORY("/dev", ROOT_UID, ROOT_GID, S_IFDIR | 0755),
    INIT_ANY_MOUNT_OPTION("/dev", "tmpfs", "mode=755", MS_NOATIME | MS_SHARED),
    INIT_ANY_NODE("/dev/kmsg", ROOT_UID, ROOT_GID, S_IFCHR | 0644, INIT_DEV_LOG_KMSG_MAJOR_NUMBER, INIT_DEV_LOG_KMSG_MINOR_NUMBER)};

const INIT_STARTUP_ANY LxssStartupWsl[] = {
    INIT_ANY_NODE("/dev/ptmx", ROOT_UID, TTY_GID, S_IFCHR | 0666, INIT_DEV_PTM_MAJOR_NUMBER, INIT_DEV_PTM_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/random", ROOT_UID, ROOT_GID, S_IFCHR | 0666, INIT_DEV_RANDOM_MAJOR_NUMBER, INIT_DEV_RANDOM_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/urandom", ROOT_UID, ROOT_GID, S_IFCHR | 0666, INIT_DEV_URANDOM_MAJOR_NUMBER, INIT_DEV_URANDOM_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/null", ROOT_UID, ROOT_GID, S_IFCHR | 0666, INIT_DEV_NULL_MAJOR_NUMBER, INIT_DEV_NULL_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/tty", ROOT_UID, TTY_GID, S_IFCHR | 0666, INIT_DEV_TTYCT_MAJOR_NUMBER, INIT_DEV_TTYCT_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/tty0", ROOT_UID, TTY_GID, S_IFCHR | 0620, INIT_DEV_TTY_MAJOR_NUMBER, INIT_DEV_TTY0_MINOR_NUMBER),
    INIT_ANY_NODE("/dev/zero", ROOT_UID, ROOT_GID, S_IFCHR | 0666, INIT_DEV_ZERO_MAJOR_NUMBER, INIT_DEV_ZERO_MINOR_NUMBER),
    INIT_ANY_NODE(LXBUS_DEVICE_NAME, ROOT_UID, ROOT_GID, S_IFCHR | 0666, INIT_DEV_LXBUS_MAJOR_NUMBER, INIT_DEV_LXBUS_MINOR_NUMBER)};

//
// Mount namespace file descriptors for VM mode.
//

int g_ElevatedMountNamespace = -1;
int g_NonElevatedMountNamespace = -1;

//
// Boot state bookkeeping.
//

extern wsl::shared::SocketChannel g_plan9ControlChannel;

void ConfigAppendNtPath(EnvironmentBlock& Environment, char* NtPath)

/*++

Routine Description:

    This routine updates the $PATH variable of the provided environment block.

Arguments:

    Environment - Supplies the environment block to update.

    NtPath - Supplies a semicolon-separated list of NT paths to translate and
        append to the $PATH variable. If no $PATH variable exists, one is
        created.

Return Value:

    None.

--*/

try
{
    auto TranslatedPath = UtilTranslatePathList(NtPath, true);
    if (!TranslatedPath.has_value())
    {
        return;
    }

    ConfigAppendToPath(Environment, TranslatedPath.value());
    return;
}
CATCH_LOG()

void ConfigAppendToPath(EnvironmentBlock& Environment, std::string_view PathElement)

/*++

Routine Description:

    This routine adds the specified path element to the $PATH variable of the
    supplied environment block.

Arguments:

    Environment - Supplies the environment block to update.

    PathElement - Supplies a path element to add to the $PATH variable. If no
        $PATH variable exists, one is created.

Return Value:

    None.

--*/

try
{
    //
    // If no PATH variable is present, create a new variable. If a PATH is
    // present, add the path element onto the end of the existing value.
    //

    auto Path = Environment.GetVariable(PATH_ENV);
    if (Path.empty())
    {
        Environment.AddVariable(PATH_ENV, PathElement);
    }
    else
    {
        std::string NewPath{Path};
        if (NewPath.back() != ':')
        {
            NewPath += ':';
        }

        NewPath += PathElement;
        Environment.AddVariable(PATH_ENV, NewPath);
    }

    return;
}
CATCH_LOG()

void ConfigHandleInteropMessage(
    wsl::shared::SocketChannel& ResponseChannel,
    wsl::shared::SocketChannel& InteropChannel,
    bool Elevated,
    gsl::span<gsl::byte> Message,
    const MESSAGE_HEADER* Header,
    const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine handles a message received from a Linux client using init's
    interop socket.

Arguments:

    ResponseChannel - Supplies channel used to send responses.

    InteropChannel - Supplies a channel to the host to be used for create
        process requests.

    Elevated - Supplies a boolean specifying if the elevated DrvFs share should be used.

    Message - Supplies the message buffer.

    Header- Supplies the message Header.

Return Value:

    None.

--*/

try
{
    switch (Header->MessageType)
    {
    case LxInitMessageCreateProcessUtilityVm:
        if (InteropChannel.Socket() > 0)
        {
            InteropChannel.SendMessage<LX_INIT_CREATE_NT_PROCESS_UTILITY_VM>(Message);
        }

        break;

    case LxInitMessageQueryDrvfsElevated:
    {
        ResponseChannel.SendResultMessage<bool>(Elevated);
        break;
    }

    case LxInitMessageQueryEnvironmentVariable:
    {
        auto* Query = gslhelpers::try_get_struct<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>(Message);
        if (!Query)
        {
            LOG_ERROR("Unexpected MessageSize {}", Message.size());
            return;
        }

        auto Value = UtilGetEnvironmentVariable(Query->Buffer);
        wsl::shared::MessageWriter<LX_INIT_QUERY_ENVIRONMENT_VARIABLE> Response(LxInitMessageQueryEnvironmentVariable);
        Response.WriteString(Value);
        ResponseChannel.SendMessage<LX_INIT_QUERY_ENVIRONMENT_VARIABLE>(Response.Span());
    }

    break;

    case LxInitMessageQueryFeatureFlags:
    {
        assert(Config.FeatureFlags.has_value());
        ResponseChannel.SendResultMessage<int32_t>(Config.FeatureFlags.value());
        break;
    }

    case LxInitMessageCreateLoginSession:
    {
        auto* CreateSession = gslhelpers::try_get_struct<LX_INIT_CREATE_LOGIN_SESSION>(Message);
        if (!CreateSession)
        {
            LOG_ERROR("Unexpected MessageSize {}", Message.size());
            return;
        }

        bool success = false;
        auto sendResponse = wil::scope_exit([&]() { ResponseChannel.SendResultMessage<bool>(success); });

        if (!Config.BootInit || Config.InitPid.value_or(0) != getpid())
        {
            LOG_ERROR("Unexpected LxInitMessageCreateLoginSession message");
        }
        else
        {
            success = CreateLoginSession(Config, CreateSession->Buffer, CreateSession->Uid);
        }

        break;
    }

    case LxInitMessageQueryNetworkingMode:
        assert(Config.NetworkingMode.has_value());
        ResponseChannel.SendResultMessage<uint8_t>(static_cast<uint8_t>(Config.NetworkingMode.value()));
        break;

    case LxInitMessageQueryVmId:
    {
        wsl::shared::MessageWriter<LX_INIT_QUERY_VM_ID> Response(LxInitMessageQueryVmId);
        if (Config.VmId.has_value())
        {
            Response.WriteString(Config.VmId.value());
        }

        ResponseChannel.SendMessage<LX_INIT_QUERY_VM_ID>(Response.Span());
        break;
    }

    default:
        LOG_ERROR("unexpected message {}", Header->MessageType);
        break;
    }
}
CATCH_LOG()

wsl::linux::WslDistributionConfig ConfigInitializeCommon(struct sigaction* SavedSignalActions)

/*++

Routine Description:

    This routine sets up common devices and mounts.

Arguments:

    SavedSignalActions - Supplies an array to save default signal actions.

Return Value:

    0 on success, -1 on failure.

--*/

{
    wil::unique_fd DevNullFd;
    unsigned int Index;

    //
    // Set the umask to 0 to ensure that devices and files that init creates
    // have the correct mode.
    //

    umask(0);

    //
    // Perform initialization required for logging to kmsg.
    //

    if (!UtilIsUtilityVm())
    {
        for (Index = 0; Index < COUNT_OF(LxssStartupLoggingWsl); Index += 1)
        {
            THROW_LAST_ERROR_IF(ConfigInitializeEntry(&LxssStartupLoggingWsl[Index]) < 0);
        }
    }
    else
    {
        for (Index = 0; Index < COUNT_OF(LxssStartupLoggingVmMode); Index += 1)
        {
            THROW_LAST_ERROR_IF(ConfigInitializeEntry(&LxssStartupLoggingVmMode[Index]) < 0);
        }
    }

    //
    // Open /dev/kmsg for logging.
    //

    THROW_LAST_ERROR_IF(InitializeLogging(true) < 0);

    //
    // Ignore all signals except SIGHUP and signals that cannot be ignored.
    //
    // N.B. Ignoring SIGCHLD automatically reaps zombie processes.
    //
    // N.B. Child processes reset signals to default before calling execv.
    //

    THROW_LAST_ERROR_IF(UtilSaveSignalHandlers(SavedSignalActions) < 0);

    THROW_LAST_ERROR_IF(UtilSetSignalHandlers(SavedSignalActions, true) < 0);

    //
    // Load the configuration file.
    //

    wsl::linux::WslDistributionConfig Config{CONFIG_FILE};

    if (getenv(LX_WSL2_SYSTEM_DISTRO_SHARE_ENV) != nullptr)
    {
        Config.GuiAppsEnabled = true;
    }

    //
    // Initialize the static entries.
    //

    for (Index = 0; Index < COUNT_OF(LxssStartupCommon); Index += 1)
    {
        THROW_LAST_ERROR_IF(ConfigInitializeEntry(&LxssStartupCommon[Index]) < 0);
    }

    //
    // Initialize WSL1 and WSL2 specific environment.
    //

    if (!UtilIsUtilityVm())
    {
        THROW_LAST_ERROR_IF(ConfigInitializeWsl() < 0);
    }

    //
    // Open /dev/null for the stdin and stdout in case libraries try to use
    // them (but keep stderr open for kmsg logging).
    //

    DevNullFd = TEMP_FAILURE_RETRY(open("/dev/null", O_RDWR));
    THROW_LAST_ERROR_IF(!DevNullFd);

    for (const auto& Fd : {STDIN_FILENO, STDOUT_FILENO})
    {
        THROW_LAST_ERROR_IF(dup2(DevNullFd.get(), Fd) < 0);
    }

    //
    // Initialize cgroups based on what the kernel supports.
    //

    ConfigInitializeCgroups(Config);

    //
    // Attempt to register the NT interop binfmt extension.
    //
    // N.B. Registration for VM mode is done by mini_init.
    //

    if ((!UtilIsUtilityVm()) && (Config.InteropEnabled))
    {
        ConfigRegisterBinfmtInterpreter();
    }

    //
    // Ensure the target for automounts exists.
    //

    if ((Config.AutoMount) || ((UtilIsUtilityVm())))
    {
        UtilMkdirPath(Config.DrvFsPrefix.c_str(), AUTO_MOUNT_PARENT_MODE, false);
    }

    //
    // Initialization successful.
    //

    return Config;
}

void ConfigInitializeX11(const wsl::linux::WslDistributionConfig& Config)
try
{
    auto socketPath = "/tmp/" X11_SOCKET_NAME;
    THROW_LAST_ERROR_IF(UtilMkdir(socketPath, 0775) < 0);

    std::string source{Config.DrvFsPrefix};
    source += WSLG_SHARED_FOLDER;
    source += "/" X11_SOCKET_NAME;
    THROW_LAST_ERROR_IF(mount(source.c_str(), socketPath, NULL, (MS_BIND | MS_REC), NULL) < 0);

    // The .X11-unix folder is mounted read-only so the socket file can't be removed.
    // It's left writable in the system distro since wslg is supposed to write to that folder to create it.
    if (WI_IsFlagClear(Config.FeatureFlags.value(), LxInitFeatureSystemDistro))
    {
        THROW_LAST_ERROR_IF(mount("none", socketPath, NULL, (MS_RDONLY | MS_REMOUNT | MS_BIND), NULL) < 0);
    }
}
CATCH_LOG()

int ConfigInitializeInstance(wsl::shared::SocketChannel& Channel, gsl::span<gsl::byte> Buffer, wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine initializes the instance's externally controlled state, which
    is received from the service.

    N.B. When setting these values errors are treated as non-fatal to account
         for unexpected distro state.

Arguments:

    MessageFd - Supplies a file descriptor to send the response message.

    Buffer - Supplies the message buffer.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // Validate input parameters.
    //

    const auto* Message = gslhelpers::try_get_struct<const LX_INIT_CONFIGURATION_INFORMATION>(Buffer);
    if (!Message)
    {
        FATAL_ERROR("Unexpected configuration size {}", Buffer.size());
    }

    //
    // Set the host name and domain name buffers.
    //

    std::string Hostname = wsl::shared::string::FromSpan(Buffer, Message->HostnameOffset);
    auto* Domainname = wsl::shared::string::FromSpan(Buffer, Message->DomainnameOffset);
    auto* WindowsHosts = wsl::shared::string::FromSpan(Buffer, Message->WindowsHostsOffset);
    auto* DistributionName = wsl::shared::string::FromSpan(Buffer, Message->DistributionNameOffset);
    auto* Plan9SocketPath = wsl::shared::string::FromSpan(Buffer, Message->Plan9SocketOffset);
    auto* Timezone = wsl::shared::string::FromSpan(Buffer, Message->TimezoneOffset);
    bool Elevated = Message->DrvfsMount == LxInitDrvfsMountElevated;

    const std::string ThreadName = std::format("{}({})", (Config.BootInit ? "init-systemd" : "init"), DistributionName);
    UtilSetThreadName(ThreadName.c_str());

    //
    // Store feature flags for future use.
    //
    // N.B. This is also stored in an environment variable so that mount.drvfs, when launched
    //      through fstab mounting below, can use that. This is needed because mount.drvfs won't
    //      be able to connect to init during this call. This environment variable is not present
    //      for user-launched processes.
    //

    Config.FeatureFlags = Message->FeatureFlags;
    char FeatureFlagsString[10];
    snprintf(FeatureFlagsString, sizeof(FeatureFlagsString), "%x", Config.FeatureFlags.value());
    if (setenv(WSL_FEATURE_FLAGS_ENV, FeatureFlagsString, 1) < 0)
    {
        LOG_ERROR("setenv failed {}", errno);
    }

    //
    // Determine the default UID which can be specified in /etc/wsl.conf.
    //

    uid_t DefaultUid = Message->DrvFsDefaultOwner;
    if (Config.DefaultUser.has_value())
    {
        passwd* PasswordEntry = getpwnam(Config.DefaultUser->c_str());
        if (PasswordEntry == nullptr)
        {
            LOG_ERROR("getpwnam({}) failed {}", Config.DefaultUser->c_str(), errno);
        }
        else
        {
            DefaultUid = PasswordEntry->pw_uid;
        }
    }

    //
    // Process the /etc/fstab file.
    //
    // N.B. This must happen before mounting DrvFs volumes because the user may
    //      have specified DrvFs mounts in /etc/fstab and they should overwrite defaults.
    //

    if (Config.MountFsTab)
    {
        ConfigMountFsTab(Elevated);
    }

    //
    // Perform additional WSL2-specific mounts.
    //

    if (UtilIsUtilityVm())
    {
        if (ConfigInitializeVmMode(Elevated, Config) < 0)
        {
            FATAL_ERROR("ConfigInitializeVmMode");
        }
    }

    if (Config.AutoMount && (Message->DrvfsMount != LxInitDrvfsMountNone))
    {
        ConfigMountDrvFsVolumes(Message->DrvFsVolumesBitmap, DefaultUid, Elevated, Config);
    }

    //
    // If a hostname was specified in /etc/wsl.conf, use it.
    //

    if (Config.HostName.has_value())
    {
        Hostname = Config.HostName.value();
        LOG_WARNING("hostname set to {} in {}", Hostname.c_str(), CONFIG_FILE);
    }

    //
    // Sanitize the hostname.
    //
    // N.B. If systemd is enabled, systemd-hostnamed will cleanup the
    // hostname, which can lead to a disconnect if that doesn't match
    // what we write in /etc/hostname & /etc/hosts, so to hostname needs
    // to be cleaned up before being passed to systemd.
    //
    // N.B. While the Windows UI doesn't let the user set an invalid hostname
    // (from systemd-hostnamed's perspective), it's possible to override that
    // via Rename-Computer.

    Hostname = wsl::shared::string::CleanHostname(Hostname);

    //
    // Update the host and domain name.
    //

    if (sethostname(Hostname.c_str(), Hostname.size()) < 0)
    {
        LOG_ERROR("sethostname({}) failed {}", Hostname.c_str(), errno);
        Hostname = wsl::shared::string::c_defaultHostName;
        if (sethostname(Hostname.c_str(), Hostname.size()) < 0)
        {
            LOG_ERROR("sethostname({}) failed {}", Hostname.c_str(), errno);
        }
    }

    if (setenv(NAME_ENV, Hostname.c_str(), 1) < 0)
    {
        LOG_ERROR("setenv({}, {}) failed {}", NAME_ENV, Hostname.c_str(), errno);
    }

    //
    // Update the domain name.
    //

    if (setdomainname(Domainname, strlen(Domainname)) < 0)
    {
        LOG_ERROR("setdomainname({}) failed {}", Domainname, errno);
    }

    //
    // Generate and write /etc/hostname.
    //

    wil::unique_fd HostnameFd{TEMP_FAILURE_RETRY(creat(HOSTNAME_FILE_PATH, HOSTNAME_FILE_MODE))};
    if (!HostnameFd)
    {
        LOG_ERROR("creat {} failed: {}", HOSTNAME_FILE_PATH, errno);
    }
    else
    {
        try
        {
            auto FileContents = std::format("{}\n", Hostname);
            if (UtilWriteStringView(HostnameFd.get(), FileContents) < 0)
            {
                LOG_ERROR("write failed {}", errno);
            }
        }
        CATCH_LOG()
    }

    HostnameFd.reset();

    //
    // Generate and write /etc/hosts.
    //

    if (Config.GenerateHosts)
    {
        wil::unique_fd HostsFd{TEMP_FAILURE_RETRY(creat(HOSTS_FILE_PATH, HOSTS_FILE_MODE))};
        if (!HostsFd)
        {
            LOG_ERROR("creat {} failed {}", HOSTS_FILE_PATH, errno);
        }
        else
        {
            try
            {
                auto FileContents = std::format(HostsFileFormatString, Hostname.c_str(), Domainname, Hostname.c_str(), WindowsHosts);
                if (UtilWriteStringView(HostsFd.get(), FileContents) < 0)
                {
                    LOG_ERROR("write failed {}", errno);
                }
            }
            CATCH_LOG()
        }
    }
    else
    {
        LOG_WARNING("{} updating disabled in {}", HOSTS_FILE_PATH, CONFIG_FILE);
    }

    //
    // Store the distribution name.
    //

    if (setenv(WSL_DISTRO_NAME_ENV, DistributionName, 1) < 0)
    {
        LOG_ERROR("setenv({}, {}, 1) failed {}", WSL_DISTRO_NAME_ENV, DistributionName, errno);
    }

    //
    // Run the Plan 9 server. This requires a DrvFs mount for the socket file,
    // so either fstab or automount must be enabled to have a chance for the
    // mount to be available.
    //
    // N.B. Failure to start the server is non-fatal.
    //

    unsigned int Plan9Port = LX_INIT_UTILITY_VM_INVALID_PORT;
    if ((WI_IsFlagClear(Config.FeatureFlags.value(), LxInitFeatureDisable9pServer)) && (Config.Plan9Enabled) &&
        (Config.AutoMount || Config.MountFsTab))
    {
        std::tie(Plan9Port, Config.Plan9ControlChannel) = StartPlan9Server(Plan9SocketPath, Config);
    }

    //
    // If the root filesystem is compressed, log a warning.
    //

    if (WI_IsFlagSet(Config.FeatureFlags.value(), LxInitFeatureRootfsCompressed))
    {
        LOG_WARNING("{} root file system is compressed, performance may be severely impacted.", DistributionName);
    }

    //
    // Update the timezone.
    //

    UpdateTimezone(Timezone, Config);

    if (Config.BootInit)
    {
        try
        {
            // Create the /run/user bind mount.
            // This mount is required because systemd will mount a tmpfs on each /run/user/<uid> folder
            // so /run/user need to be in the global mount namespace so both elevated and non elevated processes see it.
            const auto UserMountTarget = Config.DrvFsPrefix + WSLG_SHARED_FOLDER "/run/user";
            THROW_LAST_ERROR_IF(UtilMkdirPath(UserMountTarget.c_str(), 0755) < 0);
            THROW_LAST_ERROR_IF(UtilMount(UserMountTarget.c_str(), RUN_FOLDER "/" USER_MOUNT_FOLDER, nullptr, MS_BIND, nullptr) < 0)
        }
        CATCH_LOG();
    }

    //
    // Create a listening hvsocket for interop if the feature is enabled.
    //

    wil::unique_fd ListenSocket{};
    sockaddr_vm SocketAddress{};
    if (UtilIsUtilityVm() && Config.InteropEnabled)
    {
        ListenSocket = UtilListenVsockAnyPort(&SocketAddress, 1);
    }

    //
    // Send the config response to the service.
    //

    wsl::shared::MessageWriter<LX_INIT_CONFIGURATION_INFORMATION_RESPONSE> Response(LxInitMessageInitializeResponse);
    Response->Plan9Port = Plan9Port;
    Response->DefaultUid = DefaultUid;
    Response->InteropPort = ListenSocket ? SocketAddress.svm_port : LX_INIT_UTILITY_VM_INVALID_PORT;
    Response->SystemdEnabled = Config.BootInit;

    struct stat PidNamespaceInfo = {};
    THROW_LAST_ERROR_IF(stat("/proc/self/ns/pid", &PidNamespaceInfo));
    Response->PidNamespace = PidNamespaceInfo.st_ino;
    static_assert(sizeof(Response->PidNamespace) == sizeof(PidNamespaceInfo.st_ino));

    auto [Flavor, Version] = UtilReadFlavorAndVersion("/etc/os-release");
    if (Flavor.has_value())
    {
        Response.WriteString(Response->FlavorIndex, Flavor->c_str());
    }

    if (Version.has_value())
    {
        Response.WriteString(Response->VersionIndex, Version->c_str());
    }

    Channel.SendMessage<LX_INIT_CONFIGURATION_INFORMATION_RESPONSE>(Response.Span());

    //
    // Accept the interop connection.
    //

    wsl::shared::SocketChannel InteropChannel;
    if (ListenSocket)
    {
        InteropChannel = {UtilAcceptVsock(ListenSocket.get(), SocketAddress, INTEROP_TIMEOUT_MS), "Interop"};
    }

    //
    // Create a thread to handle interop requests.
    //

    InteropServer InteropServer;
    if (InteropServer.Create() < 0)
    {
        FATAL_ERROR("Could not create init interop server");
    }

    //
    // If init is not running as pid 1, create a symlink to the interop server that was created.
    //

    if (Config.InitPid.has_value())
    {
        try
        {
            std::string LinkPath = std::format(WSL_INTEROP_SOCKET_FORMAT, WSL_TEMP_FOLDER, 1, WSL_INTEROP_SOCKET);
            if (symlink(InteropServer.Path(), LinkPath.c_str()) < 0)
            {
                LOG_ERROR("symlink({}, {}) failed {}", InteropServer.Path(), LinkPath.c_str(), errno);
            }
        }
        CATCH_LOG()
    }

    UtilCreateWorkerThread(
        "Interop", [InteropChannel = std::move(InteropChannel), InteropServer = std::move(InteropServer), Elevated, &Config]() mutable {
            std::vector<gsl::byte> Buffer;
            for (;;)
            {
                wsl::shared::SocketChannel ClientChannel{InteropServer.Accept(), "InteropServer"};
                if (ClientChannel.Socket() < 0)
                {
                    continue;
                }

                auto [Message, Span] = ClientChannel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
                if (Message == nullptr)
                {
                    continue;
                }

                ConfigHandleInteropMessage(ClientChannel, InteropChannel, Elevated, Span, Message, Config);
            }
        });

    //
    // If there was a command specified in /etc/wsl.conf, run it in a child process.
    //

    if (Config.BootCommand.has_value())
    {
        UtilCreateChildProcess("BootCommand", [Command = Config.BootCommand.value(), SavedSignals = g_SavedSignalActions]() {
            //
            // Restore default signal dispositions for the child process.
            //

            THROW_LAST_ERROR_IF(UtilSetSignalHandlers(SavedSignals, false) < 0);
            THROW_LAST_ERROR_IF(UtilRestoreBlockedSignals() < 0);

            execl("/bin/sh", "sh", "-c", Command.c_str(), nullptr);
            LOG_ERROR("execl() failed, {}", errno);
        });
    }

    return 0;
}
CATCH_RETURN_ERRNO()

int ConfigInitializeVmMode(bool Elevated, wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine sets up VM Mode specific devices and mounts.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Move temporary mounts created by mini_init to their final locations.
    //
    // N.B. Failure to mount these is not fatal.
    //

    for (auto& share : g_gpuShares)
    {
        try
        {
            auto variable = LX_WSL2_GPU_SHARE_ENV + std::string{share.Name};
            auto tempMount = RemoveMountAndEnvironmentOnScopeExit(variable.c_str());
            if (tempMount && Config.GpuEnabled)
            {
                tempMount.MoveMount(share.MountPoint);
            }
        }
        CATCH_LOG()
    }

    if (Config.GpuEnabled)
    {
        ConfigApplyWindowsLibPath(Config);
    }

    try
    {
        auto tempMount = RemoveMountAndEnvironmentOnScopeExit(LX_WSL2_CROSS_DISTRO_ENV);
        if (tempMount)
        {
            const auto target = Config.DrvFsPrefix + SHARED_MOUNT_FOLDER;
            if (tempMount.MoveMount(target.c_str()))
            {
                ConfigCreateResolvConfSymlink(Config);
            }
        }
    }
    CATCH_LOG()

    try
    {
        auto tempMount = RemoveMountAndEnvironmentOnScopeExit(LX_WSL2_SYSTEM_DISTRO_SHARE_ENV);
        if (tempMount)
        {
            const auto target = Config.DrvFsPrefix + WSLG_SHARED_FOLDER;
            if (!tempMount.MoveMount(target.c_str()))
            {
                Config.GuiAppsEnabled = false;
            }
            else
            {
                Config.GuiAppsEnabled = true;

                //
                // Create a bind mount of the shared WSLg path at the expected location for x11 clients.
                //
                // N.B. If using distro init, this is done after waiting for the distro init to finish booting
                // since that will typically clear the /tmp directory.
                //

                ConfigInitializeX11(Config);

                //
                // Add environment variables to support GUI applications.
                //

                for (const auto& var : ConfigGetWslgEnvironmentVariables(Config))
                {
                    if (setenv(var.first.c_str(), var.second.c_str(), 1) < 0)
                    {
                        LOG_ERROR("setenv({}, {}) failed {}", var.first.c_str(), var.second.c_str(), errno);
                    }
                }
            }
        }
    }
    CATCH_LOG()

    try
    {
        auto tempMount = RemoveMountAndEnvironmentOnScopeExit(LX_WSL2_KERNEL_MODULES_MOUNT_ENV);
        if (tempMount)
        {
            auto target = getenv(LX_WSL2_KERNEL_MODULES_PATH_ENV);
            if (target)
            {
                unsetenv(LX_WSL2_KERNEL_MODULES_PATH_ENV);
                tempMount.MoveMount(target);
            }
        }
    }
    CATCH_LOG()

    //
    // Change the permission of some devtmpfs devices to be more permissive.
    //
    // N.B. These devices may not be present with a custom kernel config.
    //

    for (const auto* Device : {"/dev/fuse", "/dev/net/tun"})
    {
        if ((chmod(Device, 0666) < 0) && (errno != ENOENT))
        {
            LOG_ERROR("chmod({}, 0666) failed {}", Device, errno);
            return -1;
        }
    }

    //
    // Open a file descriptor to the current mount namespace.
    //

    wil::unique_fd Namespace{UtilOpenMountNamespace()};
    if (!Namespace)
    {
        return -1;
    }

    if (Elevated)
    {
        g_ElevatedMountNamespace = Namespace.release();
    }
    else
    {
        g_NonElevatedMountNamespace = Namespace.release();
    }

    return 0;
}

int ConfigInitializeWsl(void)

/*++

Routine Description:

    This routine sets up WSL-specific devices and mounts.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{
    unsigned int Index;
    int Result;

    for (Index = 0; Index < COUNT_OF(LxssStartupWsl); Index += 1)
    {
        Result = ConfigInitializeEntry(&LxssStartupWsl[Index]);
        if (Result < 0)
        {
            goto InitializeWslExit;
        }
    }

    //
    // Initialize the serial device entries.
    //

    for (Index = INIT_DEV_TTY_MINOR_NUMBER_FIRST_SERIAL; Index < INIT_DEV_TTY_MINOR_NUMBER_MAX_SERIAL; Index += 1)
    {
        auto TtySPath = std::format(INIT_DEV_TTY_SERIAL_FORMAT, (Index - INIT_DEV_TTY_MINOR_NUMBER_FIRST_SERIAL));

        Result = mknod(TtySPath.c_str(), INIT_DEV_TTY_SERIAL_MODE, makedev(INIT_DEV_TTY_MAJOR_NUMBER, Index));
        if (Result < 0)
        {
            FATAL_ERROR("mknod({}) failed {}", TtySPath, errno);
        }

        Result = chown(TtySPath.c_str(), INIT_DEV_TTY_SERIAL_UID, INIT_DEV_TTY_SERIAL_GID);
        if (Result < 0)
        {
            FATAL_ERROR("chown({}) failed {}", TtySPath, errno);
        }
    }

    Result = 0;

InitializeWslExit:
    return Result;
}

int ConfigInitializeEntry(PCINIT_STARTUP_ANY AnyEntry)

/*++

Routine Description:

    This routine creates an init startup entry.

Arguments:

    AnyEntry - Supplies the startup entry to create.

Return Value:

    0 on success, -1 on failure.

--*/

{
    PCINIT_STARTUP_DIRECTORY Directory;
    PCINIT_STARTUP_FILE File;
    PCINIT_STARTUP_MOUNT Mount;
    PCINIT_STARTUP_NODE Node;
    int Result;
    PCINIT_STARTUP_SYMBOLIC_LINK Symlink;

    switch (AnyEntry->Type)
    {
    case InitStartupTypeDirectory:
        Directory = &AnyEntry->u.Directory;
        Result = UtilMkdir(Directory->Path, Directory->Security.Mode);
        if (Result < 0)
        {
            FATAL_ERROR("Failed to create {} {}", Directory->Path, errno);
        }

        if (errno != EEXIST)
        {
            Result = chown(Directory->Path, Directory->Security.Uid, Directory->Security.Gid);
            if (Result < 0)
            {
                FATAL_ERROR("Failed to chown {} {}", Directory->Path, errno);
            }
        }

        break;

    case InitStartupTypeMount:
        Mount = &AnyEntry->u.Mount;
        Result = mount(Mount->DeviceName, Mount->MountLocation, Mount->FileSystemType, Mount->Flags & ~MS_SHARED, Mount->MountOptions);
        if (Result < 0 && !Mount->IgnoreFailure)
        {
            FATAL_ERROR("Failed to mount {} at {} as {} {}", Mount->DeviceName, Mount->MountLocation, Mount->FileSystemType, errno);
        }

        // N.B. The shared flag must be done in a followup mount() call
        if (WI_IsFlagSet(Mount->Flags, MS_SHARED))
        {
            Result = mount(nullptr, Mount->MountLocation, nullptr, MS_SHARED, nullptr);
            if (Result < 0 && !Mount->IgnoreFailure)
            {
                FATAL_ERROR("Failed to make shared mount {} {}", Mount->MountLocation, errno);
            }
        }

        break;

    case InitStartupTypeNode:
        Node = &AnyEntry->u.Node;
        Result = mknod(Node->Path, Node->Security.Mode, makedev(Node->MajorNumber, Node->MinorNumber));
        if (Result < 0)
        {
            FATAL_ERROR("Failed to create {} {}", Node->Path, errno);
        }

        Result = chown(Node->Path, Node->Security.Uid, Node->Security.Gid);
        if (Result < 0)
        {
            FATAL_ERROR("Failed to chown {} {}", Node->Path, errno);
        }

        break;

    case InitStartupTypeSymlink:
        Symlink = &AnyEntry->u.Symlink;
        Result = symlink(Symlink->Target, Symlink->Source);
        if ((Result < 0) && (errno != EEXIST))
        {
            FATAL_ERROR("Failed to create {} -> {} {}", Symlink->Source, Symlink->Target, errno);
        }

        break;

    case InitStartupTypeFile:
        File = &AnyEntry->u.File;
        Result = TEMP_FAILURE_RETRY(creat(File->FileName, File->Mode));
        if ((Result < 0) && (errno != EEXIST))
        {
            FATAL_ERROR("Failed to create {} {}", File->FileName, errno);
            goto InitializeEntryExit;
        }

        break;

    default:
        FATAL_ERROR("Unsupported Type {}", AnyEntry->Type);
    }

    Result = 0;

InitializeEntryExit:
    return Result;
}

void ConfigCreateResolvConfSymlink(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine ensures the /etc/resolv.conf symlink exists for WSL2.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if (!UtilIsUtilityVm())
    {
        return;
    }

    if (!Config.GenerateResolvConf)
    {
        LOG_WARNING("{} updating disabled in {}", RESOLV_CONF_FILE_PATH, CONFIG_FILE);

        //
        // Ensure that the symlink between /etc/resolv.conf -> /mnt/wsl/resolv.conf is removed
        //

        ConfigReconfigureResolvConfSymlink(Config);

        return;
    }

    //
    // Create a /etc/resolv.conf symlink to the file that is automatically
    // generated by WSL Core.
    //

    try
    {
        std::string Target = std::format("{}{}/{}", Config.DrvFsPrefix, SHARED_MOUNT_FOLDER, RESOLV_CONF_FILE_NAME);

        remove(RESOLV_CONF_FILE_PATH);
        if (symlink(Target.c_str(), RESOLV_CONF_FILE_PATH) < 0)
        {
            LOG_ERROR("symlink({}, {}) failed {}", Target, RESOLV_CONF_FILE_PATH, errno);
        }
    }
    CATCH_LOG()
}

int ConfigCreateResolvConfSymlinkTarget(void)

/*++

Routine Description:

    If the /etc/resolv.conf file is a symlink, this routine will recursively
    create the directory structure and target of the symlink.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{
    bool RestoreCwd = false;
    int Result;
    char SymlinkBuffer[PATH_MAX + 1];
    int SymlinkFd = -1;

    //
    // If /etc/resolv.conf is a symlink, recursively create the directory
    // structure.  If the file is not a symlink, return success. If the symlink
    // does not exist, recreate it.
    // TODO: move to std::filesystem
    //

    Result = readlink(RESOLV_CONF_FILE_PATH, SymlinkBuffer, (sizeof(SymlinkBuffer) - 1));

    if (Result < 0)
    {
        if (errno == EINVAL)
        {
            Result = 0;
            goto CreateResolvConfSymlinkTargetExit;
        }
        else if (errno == ENOENT)
        {
            Result = symlink(RESOLV_CONF_SYMLINK_TARGET, RESOLV_CONF_FILE_PATH);
            if (Result < 0)
            {
                LOG_ERROR("symlink({}, {}) failed {}", RESOLV_CONF_SYMLINK_TARGET, RESOLV_CONF_FILE_PATH, errno);

                goto CreateResolvConfSymlinkTargetExit;
            }

            Result = readlink(RESOLV_CONF_FILE_PATH, SymlinkBuffer, (sizeof(SymlinkBuffer) - 1));
        }

        if (Result < 0)
        {
            LOG_ERROR("readlink({}) failed {}", RESOLV_CONF_FILE_PATH, errno);
            goto CreateResolvConfSymlinkTargetExit;
        }
    }

    //
    // Null-terminate the symlink buffer string.
    //

    SymlinkBuffer[Result] = '\0';

    //
    // Set current working directory to the folder that contains the resolv.conf
    // symlink.
    //
    // N.B. This is so creating the target of the symlinks will work since they
    //      may use relative symlinks.
    //

    Result = chdir(ETC_FOLDER);
    if (Result < 0)
    {
        LOG_ERROR("chdir {} failed {}", ETC_FOLDER, errno);
        goto CreateResolvConfSymlinkTargetExit;
    }

    RestoreCwd = true;

    //
    // Check if the symlink target exists, if the file exists return success. If
    // it does not exist, recursively create the directory structure.
    //

    Result = access(SymlinkBuffer, W_OK);
    if (Result == 0)
    {
        goto CreateResolvConfSymlinkTargetExit;
    }
    else if (errno != ENOENT)
    {
        Result = -1;
        LOG_ERROR("access {} W_OK failed {}", SymlinkBuffer, errno);
        goto CreateResolvConfSymlinkTargetExit;
    }

    //
    // Recursively create the directory structure.
    //

    Result = UtilMkdirPath(SymlinkBuffer, RESOLV_CONF_DIRECTORY_MODE, true);

    //
    // The symlink target itself does not need to be created here, as it will
    // be created when the symlink is opened later.
    //

    Result = 0;

CreateResolvConfSymlinkTargetExit:
    if (RestoreCwd != false)
    {
        if (chdir(DEFAULT_CWD) < 0)
        {
            LOG_ERROR("chdir({}) failed {}", DEFAULT_CWD, errno);
        }
    }

    if (SymlinkFd != -1)
    {
        CLOSE(SymlinkFd);
    }

    return Result;
}

int ConfigReconfigureResolvConfSymlink(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    Checks the value of the Config.GenerateResolvConf and removes the symlink (if one has been set previously)

Arguments:

    Config - Supplies the Distribution Configuration.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int Result;
    char SymLinkBuffer[PATH_MAX + 1];

    // check if /etc/resolv.conf is symlink
    // TODO: move to std::filesystem.
    Result = readlink(RESOLV_CONF_FILE_PATH, SymLinkBuffer, (sizeof(SymLinkBuffer) - 1));
    if (Result < 0)
    {
        if (errno == EINVAL || errno == ENOENT)
        {
            Result = 0;
        }
        else
        {
            LOG_ERROR("readlink({}) failed {}", RESOLV_CONF_FILE_PATH, errno);
        }

        return Result;
    }

    // null-terminate the symlink buffer string
    SymLinkBuffer[Result] = '\0';

    // recreate the location of [automount root]/wsl/resolv.conf
    auto target = Config.DrvFsPrefix + std::string{RESOLV_CONF_SYMLINK_WSL_MOUNT_SUFFIX};

    // check if the symlink is pointing to /mnt/wsl/resolv.conf created by wslcore
    // do not interfere with symlinks set by other networking management processes (ie. resolvconf, NetworkManager, etc.)
    if (std::string_view{SymLinkBuffer} == target)
    {
        // generateResolveConf setting has changed, remove symlink and restore if specified
        Result = remove(RESOLV_CONF_FILE_PATH);
        if (Result < 0)
        {
            LOG_ERROR("remove({}) failed {}", RESOLV_CONF_FILE_PATH, errno);
        }
    }

    return Result;
}

EnvironmentBlock ConfigCreateEnvironmentBlock(const PLX_INIT_CREATE_PROCESS_COMMON Common, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine creates the environment block to be used when launching a new
    process.

Arguments:

    Common - Supplies a pointer to the common create process message data.

Return Value:

    An environment block.

--*/

{
    //
    // Initialize the environment block.
    //

    auto Buffer = (char*)Common + Common->EnvironmentOffset;
    EnvironmentBlock Environment(Buffer, Common->EnvironmentCount);

    //
    // Add environment variables to support GUI applications.
    //
    // N.B. This must be done before processing WSLENV so the user can override
    //      these values if desired.
    //

    if (Config.GuiAppsEnabled)
    {
        for (const auto& Var : ConfigGetWslgEnvironmentVariables(Config))
        {
            Environment.AddVariable(Var.first, Var.second);
        }
    }

    //
    // Add each Windows environment variable from WSLENV to the environment block.
    //
    // N.B. Failure to parse WSLENV is non-fatal.
    //

    Buffer = (char*)Common + Common->NtEnvironmentOffset;
    auto NtEnvironment = UtilParseWslEnv(Buffer);
    if (!NtEnvironment.empty())
    {
        for (size_t Index = 0;;)
        {
            Buffer = NtEnvironment.data() + Index;
            auto Length = strnlen(Buffer, NtEnvironment.size() - Index);
            if (Length == 0)
            {
                break;
            }

            auto Value = strchr(Buffer, '=');
            if (Value != NULL)
            {
                *Value = '\0';
                Value += 1;
                Environment.AddVariable(Buffer, Value);
            }

            Index += Length + 1;
        }
    }

    //
    // Add the GPU library to the $PATH variable. This is done because some GPU
    // vendors ship small utilities along with their usermode drivers.
    //

    if (UtilIsUtilityVm())
    {
        if (Config.AppendGpuLibPath && Config.GpuEnabled)
        {
            ConfigAppendToPath(Environment, LXSS_LIB_PATH);
        }
    }

    //
    // Translate the NT path into a list of Linux paths and add it to the PATH
    // environment variable. Individual path elements that fail to translate
    // are skipped.
    //
    // N.B. Failure to append the NT path is non-fatal.
    //

    Buffer = reinterpret_cast<char*>(Common) + Common->NtPathOffset;
    if ((Config.InteropAppendWindowsPath) && (*Buffer != '\0'))
    {
        ConfigAppendNtPath(Environment, Buffer);
    }

    return Environment;
}

std::set<std::pair<unsigned int, std::string>> ConfigGetMountedDrvFsVolumes(void)

/*++

Routine Description:

    This routine returns a bitmap indicating which Windows drive letters are
    already mounted.

    N.B. If this function fails, it just returns 0, since failure is not
         considered fatal here.

Arguments:

    None.

Return Value:

    The bitmap of mounted drives.

--*/

{
    std::set<std::pair<unsigned int, std::string>> MountPoints;
    mountutil::MountEnum MountEnum;
    while (MountEnum.Next())
    {
        //
        // Do not consider bind mounts.
        //

        if (strcmp(MountEnum.Current().Root, "/") != 0)
        {
            continue;
        }

        //
        // Extract the correct mount source depending on whether this is 9p
        // (WSL2) or DrvFs (WSL1). For virtio-9p, the entry's mount source
        // will just be "drvfs" or "drvfsa", so it must be extracted from the
        // aname (this works for hvsocket-9p too).
        // N.B. UtilParsePlan9MountSource always returns a canonicalized path.
        //

        std::string MountSource;
        if (strcmp(MountEnum.Current().FileSystemType, PLAN9_FS_TYPE) == 0)
        {
            MountSource = UtilParsePlan9MountSource(MountEnum.Current().SuperOptions);
            if (MountSource.empty())
            {
                continue;
            }
        }
        else if (strcmp(MountEnum.Current().FileSystemType, DRVFS_FS_TYPE) == 0)
        {
            MountSource = MountEnum.Current().Source;
            UtilCanonicalisePathSeparator(MountSource, PATH_SEP_NT);
        }
        else if (strcmp(MountEnum.Current().FileSystemType, VIRTIO_FS_TYPE) == 0)
        {
            MountSource = UtilParseVirtiofsMountSource(MountEnum.Current().Source);
        }
        else
        {
            continue;
        }

        auto letter = ConfigGetDriveLetter(MountSource);
        if (letter.has_value())
        {
            MountPoints.emplace(letter.value(), MountEnum.Current().MountPoint);
        }
    }

    return MountPoints;
}

std::vector<std::pair<std::string, std::string>> ConfigGetWslgEnvironmentVariables(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine returns the environment variables needed by WSLg.

Arguments:

    None.

Return Value:

    A list of environment variables.

--*/

{
    std::string WaylandPath = std::format("{}{}/{}", Config.DrvFsPrefix, WSLG_SHARED_FOLDER, WAYLAND_RUNTIME_DIR);
    std::string PulsePath = std::format("unix:{}{}/{}", Config.DrvFsPrefix, WSLG_SHARED_FOLDER, PULSE_SERVER_NAME);
    return std::vector<std::pair<std::string, std::string>>{
        {XDG_RUNTIME_DIR_ENV, std::move(WaylandPath)},
        {X11_DISPLAY_ENV, X11_DISPLAY_VALUE},
        {WAYLAND_DISPLAY_ENV, WAYLAND_DISPLAY_VALUE},
        {PULSE_SERVER_ENV, std::move(PulsePath)},
        {LX_WSL2_GUI_APP_SUPPORT_ENV, "1"}};
}

void ConfigInitializeCgroups(wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This parses the /proc/cgroups file and mounts enabled cgroups.

    N.B. This routine was modeled after the cgroupfs-mount script.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

try
{
    std::vector<std::string> DisabledControllers;

    if (UtilIsUtilityVm())
    {
        if (Config.CGroup == WslDistributionConfig::CGroupVersion::v1)
        {
            auto commandLine = UtilReadFileContent("/proc/cmdline");
            auto position = commandLine.find(CGROUPS_NO_V1);
            if (position != std::string::npos)
            {
                auto list = commandLine.substr(position + sizeof(CGROUPS_NO_V1) - 1);
                auto end = list.find_first_of(" \n");
                if (end != std::string::npos)
                {
                    list = list.substr(0, end);
                }

                if (list == "all")
                {
                    LOG_WARNING("Distribution has cgroupv1 enabled, but kernel command line has {}all. Falling back to cgroupv2", CGROUPS_NO_V1);
                    Config.CGroup = WslDistributionConfig::CGroupVersion::v2;
                }
                else
                {
                    DisabledControllers = wsl::shared::string::Split(list, ',');
                }
            }
        }

        if (Config.CGroup == WslDistributionConfig::CGroupVersion::v1)
        {
            THROW_LAST_ERROR_IF(mount("tmpfs", CGROUP_MOUNTPOINT, "tmpfs", (MS_NOSUID | MS_NODEV | MS_NOEXEC), "mode=755") < 0);
        }

        const auto Target = Config.CGroup == WslDistributionConfig::CGroupVersion::v1 ? CGROUP_MOUNTPOINT "/unified" : CGROUP_MOUNTPOINT;
        THROW_LAST_ERROR_IF(
            UtilMount(CGROUP2_DEVICE, Target, CGROUP2_DEVICE, (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME), "nsdelegate") < 0);

        if (Config.CGroup == WslDistributionConfig::CGroupVersion::v2)
        {
            return;
        }
    }
    else
    {
        THROW_LAST_ERROR_IF(mount("tmpfs", CGROUP_MOUNTPOINT, "tmpfs", (MS_NOSUID | MS_NODEV | MS_NOEXEC), "mode=755") < 0);
    }

    //
    // Mount cgroup v1 when running in WSL1 mode or when a WSL2 distro has automount.cgroups=v1 specified.
    //
    // Open the /proc/cgroups file and parse each line, ignoring malformed
    // lines and disabled controllers.
    //

    wil::unique_file Cgroups{fopen(CGROUPS_FILE, "r")};
    THROW_LAST_ERROR_IF(!Cgroups);

    ssize_t BytesRead;
    char* Line = nullptr;
    auto LineCleanup = wil::scope_exit([&]() { free(Line); });
    size_t LineLength = 0;
    while ((BytesRead = getline(&Line, &LineLength, Cgroups.get())) != -1)
    {
        char* Subsystem = nullptr;
        bool Enabled = false;
        if ((UtilParseCgroupsLine(Line, &Subsystem, &Enabled) < 0) || (Enabled == false) ||
            std::find(DisabledControllers.begin(), DisabledControllers.end(), Subsystem) != DisabledControllers.end())

        {
            continue;
        }

        auto Target = std::format("{}/{}", CGROUP_MOUNTPOINT, Subsystem);
        THROW_LAST_ERROR_IF(
            UtilMount(CGROUP_DEVICE, Target.c_str(), CGROUP_DEVICE, (MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME), Subsystem) < 0);
    }
}
CATCH_LOG()

std::optional<unsigned int> ConfigGetDriveLetter(std::string_view MountSource)

/*++

Routine Description:

    This routine extracts the drive letter from a mount source.

    N.B. Recognized formats are "X:", "X:\134" (the escape sequence for a
         backslash used in /proc/self/mounts) and "X:/", where X may be
         lowercase or uppercase.

Arguments:

    MountSource - Supplies the mount source.

Return Value:

    The drive letter index as an integer (0 is 'A', 1 is 'B' and so on).

--*/

{
    //
    // The length must be 2 or 3 and the second character must always be ':'.
    //

    if ((MountSource.length() < 2) || (MountSource.length() > 3) || (MountSource[1] != ':'))
    {
        return {};
    }

    //
    // If there are three characters, the third one must be a path separator.
    //

    if (MountSource.length() == 3 && MountSource[2] != '/' && MountSource[2] != '\\')
    {
        return {};
    }

    //
    // Extract the drive letter from the first character.
    //

    if ((MountSource[0] >= 'a') && (MountSource[0] <= 'z'))
    {
        return MountSource[0] - 'a';
    }
    else if ((MountSource[0] >= 'A') && (MountSource[0] <= 'Z'))
    {
        return MountSource[0] - 'A';
    }

    return {};
}

void ConfigMountDrvFsVolumes(unsigned int DrvFsVolumes, uid_t OwnerUid, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine mounts the specified DrvFs volumes.

Arguments:

    DrvFsVolumes - Supplies a bitmap that contains the indices of DrvFs volumes
        to mount.

    OwnerUid - Supplies the owner uid to use.

    Admin - Supplies an optional boolean to specify if the admin or non-admin
        server should be used.

Return Value:

    None.

--*/

try
{
    if (DrvFsVolumes == 0)
    {
        return;
    }

    //
    // If fstab was processed, exclude already mounted volumes.
    //

    std::set<std::pair<unsigned int, std::string>> MountedVolumes;
    if (Config.MountFsTab)
    {
        MountedVolumes = ConfigGetMountedDrvFsVolumes();
    }

    //
    // Attempt to determine the owner gid to use.
    //
    // N.B. If no entry is found, root is used as the owner gid.
    //

    auto OwnerGid = ROOT_GID;
    auto Password = getpwuid(OwnerUid);
    if (Password != nullptr)
    {
        OwnerGid = Password->pw_gid;
    }

    //
    // Initialize the mount options.
    //
    // N.B. If the options weren't specified, ConfigDrvFsOptions will be an
    //      empty string. Since DrvFs ignores empty mount options, the extra
    //      comma on the end in that case is not a problem.
    //

    std::string Options =
        std::format("noatime,uid={},gid={},{}", OwnerUid, OwnerGid, Config.DrvFsOptions.has_value() ? Config.DrvFsOptions->c_str() : "");

    //
    // Iterate over the bitmap and attempt to create a DrvFs mount for each
    // drive letter.
    //
    // N.B. __builtin_ffsll returns a one-based index.
    //

    char Source[] = DRVFS_SOURCE;
    for (int Index = __builtin_ffsll(DrvFsVolumes); Index != 0; Index = __builtin_ffsll(DrvFsVolumes))
    {
        //
        // Mask out the current Index.
        //

        Index -= 1;
        DrvFsVolumes ^= (1 << Index);

        //
        // If this drive is already mounted on the same mountpoint, skip.
        //

        auto Target = std::format("{}{:c}", Config.DrvFsPrefix, 'a' + Index);
        if (MountedVolumes.contains(std::make_pair(Index, Target.c_str())))
        {
            LOG_WARNING("{} already mounted, skipping...", Target);
            continue;
        }

        //
        // Create the target directory and attempt the mount.
        //

        if (UtilMkdir(Target.c_str(), DRVFS_TARGET_MODE) < 0)
        {
            continue;
        }

        Source[0] = 'A' + Index;
        if (MountDrvfs(Source, Target.c_str(), Options.c_str(), Admin, Config) < 0)
        {
            EMIT_USER_WARNING(wsl::shared::Localization::MessageDrvfsMountFailed(Source));
        }
    }
}
CATCH_LOG()

static void ConfigApplyWindowsLibPath(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine creates a file for the GNU loader to include in its
    library search paths, located under /etc/ld.so.conf.d/.

    After writing this file, /sbin/ldconfig will be invoked to update the cache.

    N.B. Failures during this function are not fatal to instance start.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    const char* const LdConfigArgv[] = {LDCONFIG_COMMAND, nullptr};

    if (!Config.LinkOsLibs)
    {
        return;
    }

    wil::unique_fd Fd{TEMP_FAILURE_RETRY(open(WINDOWS_LD_CONF_FILE, (O_CREAT | O_RDWR | O_TRUNC), WINDOWS_LD_CONF_FILE_MODE))};
    if (!Fd)
    {
        LOG_ERROR("open {} failed {}", WINDOWS_LD_CONF_FILE, errno);
        return;
    }

    std::string_view Buffer{WindowsLibSearchFileHeaderString};
    if (UtilWriteStringView(Fd.get(), Buffer) < 0)
    {
        LOG_ERROR("write failed {}", errno);
        return;
    }

    Buffer = LXSS_LIB_PATH;
    if (UtilWriteStringView(Fd.get(), Buffer) < 0)
    {
        LOG_ERROR("write failed {}", errno);
        return;
    }

    if (UtilCreateProcessAndWait(LdConfigArgv[0], LdConfigArgv) < 0)
    {
        LOG_ERROR("Processing ldconfig failed");
    }
}

void ConfigMountFsTab(bool Elevated)

/*++

Routine Description:

    This routine runs mount -a to process the /etc/fstab file.

    N.B. Failures during this function are not fatal to instance start.

Arguments:

    Elevated - True if the plan9 drvfs entries should use the elevated plan9 server.

Return Value:

    None.

--*/

{
    //
    // Note: The WSL_DRVFS_ELEVATED_ENV variable is used because the interop server isn't running yet.
    //

    const char* const Argv[] = {MOUNT_COMMAND, MOUNT_FSTAB_ARG, nullptr};
    if (UtilCreateProcessAndWait(Argv[0], Argv, nullptr, {{WSL_DRVFS_ELEVATED_ENV, Elevated ? "1" : "0"}}) < 0)
    {
        auto message = wsl::shared::Localization::MessageFstabMountFailed();
        LOG_ERROR("{}", message.c_str());

        EMIT_USER_WARNING(std::move(message));
    }
}

int ConfigRegisterBinfmtInterpreter(void)

/*++

Routine Description:

    This routine registers the binfmt extension for interop.

Arguments:

    None.

Return Value:

    0 on success, -1 on failure.

--*/

{
    //
    // Register the interop binfmt extension.
    //

    wil::unique_fd Fd{TEMP_FAILURE_RETRY(open(BINFMT_MISC_REGISTER_FILE, O_WRONLY))};
    if (!Fd)
    {
        LOG_ERROR("open " BINFMT_MISC_REGISTER_FILE " failed {}", errno);
        return -1;
    }

    std::string_view Buffer{BINFMT_INTEROP_REGISTRATION_STRING(LX_INIT_BINFMT_NAME) "\n"};
    int Result = UtilWriteStringView(Fd.get(), Buffer);
    if (Result < 0)
    {
        LOG_ERROR("binfmt registration failed {}", errno);
    }

    return Result;
}

int ConfigRemountDrvFs(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This remounts DrvFs volumes in the appropriate mount namespace
    and the result on the channel.

Arguments:

    Buffer - Supplies a buffer to the LX_INIT_MOUNT_DRVFS message.

    ResultChannel - Supplies the file descriptor to write the result to.

Return Value:

    0 on success, -1 on failure.

--*/
{
    Channel.SendResultMessage<int32_t>(ConfigRemountDrvFsImpl(Buffer, Config));

    return 0;
}

int ConfigRemountDrvFsImpl(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This remounts DrvFs volumes in the appropriate mount namespace.

Arguments:

    Buffer - Supplies a buffer to the LX_INIT_MOUNT_DRVFS message.

    Config - Supplies the distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // This method is only valid for VM mode.
    //

    if (!UtilIsUtilityVm())
    {
        return -1;
    }

    const auto* Message = gslhelpers::try_get_struct<LX_INIT_MOUNT_DRVFS>(Buffer);
    if (!Message)
    {
        LOG_ERROR("Unexpected sizeof for LX_INIT_MOUNT_DRVFS: {}u", Buffer.size());
        return -1;
    }

    if (Message->Admin ? (g_ElevatedMountNamespace != -1) : (g_NonElevatedMountNamespace != -1))
    {
        LOG_ERROR("{} namespace already initialized", Message->Admin ? "Admin" : "Non-Admin");
        return -1;
    }

    //
    // Read the mountinfo file for the namespace that is already configured.
    // This contains all mounts from /etc/fstab as well as the initial drives
    // that were mounted when the instance was created.
    //

    wil::unique_file MountInfo{fopen(MOUNT_INFO_FILE, "r")};
    if (!MountInfo)
    {
        LOG_ERROR("fopen failed {}", errno);
        return -1;
    }

    std::string FileContents = UtilReadFile(MountInfo.get());

    wil::unique_fd OriginalNamespace{UtilOpenMountNamespace()};
    if (!OriginalNamespace)
    {
        return -1;
    }

    auto RestoreNamespace = wil::scope_exit([&]() {
        if (setns(OriginalNamespace.get(), CLONE_NEWNS) < 0)
        {
            LOG_ERROR("restoring mount namespace failed {}", errno);
        }
    });

    //
    // Configure the new mount namespace.
    //

    if (unshare(CLONE_NEWNS) < 0)
    {
        LOG_ERROR("unshare failed {}", errno);
        return -1;
    }

    wil::unique_fd NewNamespace{UtilOpenMountNamespace()};
    if (!NewNamespace)
    {
        return -1;
    }

    if (Message->Admin)
    {
        g_ElevatedMountNamespace = NewNamespace.release();
    }
    else
    {
        g_NonElevatedMountNamespace = NewNamespace.release();
    }

    //
    // Parse the mountinfo file get a list of all the drvfs mounts.
    //

    std::vector<MOUNT_ENTRY> DrvfsMounts;
    for (char *Sp1, *Info = strtok_r(FileContents.data(), "\n", &Sp1); Info != nullptr; Info = strtok_r(NULL, "\n", &Sp1))
    {
        MOUNT_ENTRY MountEntry;
        if (MountParseMountInfoLine(Info, &MountEntry) < 0)
        {
            return -1;
        }

        //
        // Bind mounts which have a root other than / are currently not supported.
        //
        // TODO_LX: Support bind mounts.
        //

        if (strcmp(MountEntry.Root, "/") != 0)
        {
            continue;
        }

        if (strcmp(MountEntry.FileSystemType, PLAN9_FS_TYPE) == 0)
        {

            //
            // Ensure that only drvfs mounts are re-mounted. This avoids unmounting sharefs mounts (used for mounting gpu libs and drivers).
            //

            auto Plan9Source = UtilParsePlan9MountSource(MountEntry.SuperOptions);
            if (Plan9Source.empty() || !ConfigGetDriveLetter(Plan9Source).has_value())
            {
                continue;
            }
        }
        else if (strcmp(MountEntry.FileSystemType, VIRTIO_FS_TYPE) != 0)
        {
            continue;
        }

        DrvfsMounts.emplace_back(std::move(MountEntry));
    }

    //
    // Unmount the existing drvfs mounts in reverse order, then remount the new version.
    //

    for (auto ReverseIterator = DrvfsMounts.rbegin(); ReverseIterator != DrvfsMounts.rend(); ReverseIterator++)
    {
        const auto* MountPoint = (*ReverseIterator).MountPoint;
        if (umount2(MountPoint, MNT_DETACH) < 0)
        {
            LOG_ERROR("umount2({}) failed {}", MountPoint, errno);
        }
    }

    std::bitset<32> volumesToMount(Message->VolumesToMount);
    std::bitset<32> unreadableVolumes(Message->UnreadableVolumes);

    std::string NewMountOptions;
    for (const auto& MountEntry : DrvfsMounts)
    {
        if (strcmp(MountEntry.FileSystemType, PLAN9_FS_TYPE) == 0)
        {
            const char* NewSource = MountEntry.Source;
            auto Plan9Source = UtilParsePlan9MountSource(MountEntry.SuperOptions);
            if (Plan9Source.empty())
            {
                continue;
            }

            auto driveIndex = ConfigGetDriveLetter(Plan9Source);
            if (driveIndex.has_value())
            {
                //
                // This is drive mount. Remount only if the drive is actually readable.
                //

                if (unreadableVolumes[driveIndex.value()])
                {
                    //
                    // This drive is not readable, don't try to mount it.
                    //

                    LOG_WARNING("Drvfs mount '{}' is not readable, skipping mount", Plan9Source);
                    continue;
                }

                volumesToMount[driveIndex.value()] = false;
            }

            //
            // Construct new Plan9 mount options based on the existing mount.
            //

            NewMountOptions = MountEntry.MountOptions;
            NewMountOptions += ',';
            if (WSL_USE_VIRTIO_9P(Config))
            {
                //
                // Check if the existing mount is a drvfs mount that needs to be remounted.
                //

                auto Tag = Message->Admin ? LX_INIT_DRVFS_VIRTIO_TAG : LX_INIT_DRVFS_ADMIN_VIRTIO_TAG;
                if (strcmp(MountEntry.Source, Tag) != 0)
                {
                    continue;
                }

                NewSource = Message->Admin ? LX_INIT_DRVFS_ADMIN_VIRTIO_TAG : LX_INIT_DRVFS_VIRTIO_TAG;
            }

            //
            // Remove the transport-related mount options.
            //

            std::string_view SuperOptions = MountEntry.SuperOptions;
            while (!SuperOptions.empty())
            {
                auto Option = UtilStringNextToken(SuperOptions, ",");
                if (wsl::shared::string::StartsWith(Option, "trans=") || wsl::shared::string::StartsWith(Option, "rfd=") ||
                    wsl::shared::string::StartsWith(Option, "wfd=") || wsl::shared::string::StartsWith(Option, "msize="))
                {
                    continue;
                }

                NewMountOptions += Option;
                NewMountOptions += ',';
            }

            MountPlan9Filesystem(NewSource, MountEntry.MountPoint, NewMountOptions.c_str(), Message->Admin, Config);
        }
        else if (strcmp(MountEntry.FileSystemType, VIRTIO_FS_TYPE) == 0)
        {
            std::string_view Source = MountEntry.Source;
            std::string_view OldTag = Message->Admin ? LX_INIT_DRVFS_VIRTIO_TAG : LX_INIT_DRVFS_ADMIN_VIRTIO_TAG;
            if (!wsl::shared::string::StartsWith(Source, OldTag))
            {
                continue;
            }

            RemountVirtioFs(MountEntry.Source, MountEntry.MountPoint, MountEntry.MountOptions, Message->Admin);
        }
        else
        {
            LOG_ERROR("Unexpected fstype {}", MountEntry.FileSystemType);
        }
    }

    // It's possible that some drives are only visible to one namespace and not the other
    // (for instance if only an elevated token has read access to a drive).
    // If that's the case, those drives might not have been mounted previously, so
    // mount any drive that hasn't been found in MountInfo.
    if (Config.AutoMount)
    {
        ConfigMountDrvFsVolumes(volumesToMount.to_ulong(), Message->DefaultOwnerUid, Message->Admin, Config);
    }

    return 0;
}
CATCH_RETURN_ERRNO()

int ConfigSetMountNamespace(bool Elevated)

/*++

Routine Description:

    This routine sets the mount namespace of the caller.

Arguments:

    Elevated - Supplies true if the client represents an elevated Windows process, false otherwise.

Return Value:

    The file descriptor representing the mount namespace on success, -1 on failure.

--*/

{
    if (!UtilIsUtilityVm())
    {
        return -1;
    }

    auto Namespace = Elevated ? g_ElevatedMountNamespace : g_NonElevatedMountNamespace;
    if (Namespace == -1)
    {
        LOG_ERROR("{} namespace has not been initialized", Elevated ? "Admin" : "Non-Admin");
        return -1;
    }

    if (setns(Namespace, CLONE_NEWNS) < 0)
    {
        LOG_ERROR("setns failed {}", errno);
        return -1;
    }

    return Namespace;
}

void ConfigUpdateLanguage(EnvironmentBlock& Environment)

/*++

Routine Description:

    This routine queries the contents of the /etc/default/locale text file and
    if present updates the $LANG environment variable in the environment block.

Arguments:

    Environment - Supplies the environment block pointer to update.

Return Value:

    None.

--*/

try
{
    //
    // Attempt to open the /etc/default/locale file. If the file does not exist
    // then the $LANG environment variable will not be updated.
    //
    // N.B. This file is being opened by root. The only user-visible content
    //      will be the contents of the last line of the file that contains
    //      "LANG=".
    //

    wil::unique_file LocaleFile{fopen(LOCALE_FILE_PATH, "r")};
    if (!LocaleFile)
    {
        if (errno != ENOENT)
        {
            LOG_ERROR("fopen({}) failed {}", LOCALE_FILE_PATH, errno);
        }

        return;
    }

    // TODO: Move to std::regex

    //
    // Parse the file line-by-line looking for the "LANG=" string.
    //

    char* Line = nullptr;
    auto freeLine = wil::scope_exit([&Line]() { free(Line); });

    size_t LineLength = 0;
    while (getline(&Line, &LineLength, LocaleFile.get()) != -1)
    {
        //
        // Handle comments by replacing the first comment character with a null
        // terminator, thus ending the string.
        //

        auto SpecialCharacter = strchr(Line, '#');
        if (SpecialCharacter != nullptr)
        {
            *SpecialCharacter = '\0';
        }

        //
        // If the current line contains the "LANG=" string, update the
        // environment block with the remainder of the line.
        //
        // N.B. No validation is done on the contents of the string. If the
        //      file contains multiple lines containing "LANG=" the last will
        //      be used.
        //

        auto Content = strstr(Line, LANG_ENV "=");
        if (Content != nullptr)
        {
            Content += sizeof(LANG_ENV);

            //
            // Replace newline character with a null terminator.
            //

            SpecialCharacter = strchr(Content, '\n');
            if (SpecialCharacter != nullptr)
            {
                *SpecialCharacter = '\0';
            }

            Environment.AddVariable(LANG_ENV, Content);
        }
    }

    return;
}
CATCH_LOG()

void ConfigUpdateNetworkInformation(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine updates the instance's network information by writing to
    /etc/resolv.conf.

Arguments:

    Buffer - Supplies the message buffer.

    Config - Supplies the WSL distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    if (!Config.GenerateResolvConf)
    {
        LOG_WARNING("{} updating disabled in {}", RESOLV_CONF_FILE_PATH, CONFIG_FILE);
        return;
    }

    //
    // Validate input parameters.
    //

    auto* Message = gslhelpers::try_get_struct<LX_INIT_NETWORK_INFORMATION>(Buffer);
    if (!Message)
    {
        LOG_ERROR("Unexpected network information size {}", Buffer.size());
        return;
    }

    //
    // Write the contents to /etc/resolv.conf.
    //

    if (ConfigCreateResolvConfSymlinkTarget() < 0)
    {
        return;
    }

    THROW_LAST_ERROR_IF(UtilMkdir(RESOLV_CONF_FOLDER, RESOLV_CONF_DIRECTORY_MODE) < 0);

    wil::unique_fd Fd{TEMP_FAILURE_RETRY(open(RESOLV_CONF_FILE_PATH, (O_CREAT | O_RDWR | O_TRUNC), RESOLV_CONF_FILE_MODE))};
    THROW_LAST_ERROR_IF(!Fd);

    const char* Header = wsl::shared::string::FromSpan(Buffer, Message->FileHeaderIndex);
    if (Header)
    {
        THROW_LAST_ERROR_IF(UtilWriteStringView(Fd.get(), Header) < 0);
    }

    const char* Content = wsl::shared::string::FromSpan(Buffer, Message->FileContentsIndex);
    if (Content)
    {
        THROW_LAST_ERROR_IF(UtilWriteStringView(Fd.get(), Content) < 0);
    }
    else
    {
        LOG_ERROR("/etc/resolv.conf unexpectedly empty");
    }
}
CATCH_LOG()

bool CreateLoginSession(const wsl::linux::WslDistributionConfig& Config, const char* Username, uid_t Uid)
/*++

Routine Description:

    Create a systemd login session for the given user.

Arguments:

    Config - Supplies the WSL distribution configuration.

    Username - Supplies session username.

    Uid - Supplies the session UID.

Return Value:

    true on success, false on failure.

--*/
try
{
    static std::mutex LoginSessionsLock;
    static std::map<uid_t, int> LoginSessions;

    // Keep track of login sessions that have been created.
    LoginSessionsLock.lock();
    auto Unlock = wil::scope_exit([&]() { LoginSessionsLock.unlock(); });
    if (LoginSessions.contains(Uid))
    {
        return true;
    }

    int LoginLeader;
    const int Result = forkpty(&LoginLeader, nullptr, nullptr, nullptr);
    if (Result < 0)
    {
        LOG_ERROR("forkpty failed {}", errno);
        return false;
    }
    else if (Result == 0)
    {
        Unlock.reset();
        _exit(execl("/bin/login", "/bin/login", "-f", Username, nullptr));
    }

    LoginSessions.emplace(Uid, LoginLeader);

    //
    // N.B. Init needs to not ignore SIGCHLD so it can wait for the child process.
    //
    signal(SIGCHLD, SIG_DFL);
    auto restoreDisposition = wil::scope_exit([]() { signal(SIGCHLD, SIG_IGN); });

    if (Config.BootInitTimeout > 0)
    {
        auto cmd = std::format("systemctl is-active user@{}.service", Uid);
        try
        {
            return wsl::shared::retry::RetryWithTimeout<bool>(
                [&]() {
                    std::string Output;
                    auto exitCode = UtilExecCommandLine(cmd.c_str(), &Output, 0, false);
                    if (exitCode == 0) // is-active returns 0 if the unit is active.
                    {
                        return true;
                    }
                    else if (Output == "failed\n")
                    {
                        LOG_ERROR("{} returned: {}", cmd, Output);
                        return false;
                    }

                    THROW_ERRNO(EAGAIN);
                },
                std::chrono::milliseconds{250},
                std::chrono::milliseconds{Config.BootInitTimeout});
        }
        catch (...)
        {
            LOG_ERROR("Timed out waiting for user session for uid={}", Uid);
            return false;
        }
    }

    return true;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}
