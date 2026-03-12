/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    init.c

Abstract:

    This file contains the lx init implementation.

--*/

#include <cassert>
#include <sys/eventfd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <pty.h>
#include <utmp.h>
#include <libgen.h>
#include <grp.h>
#include <sysexits.h>
#include <iostream>
#include <cstddef>
#include <lxbusapi.h>
#include "p9tracelogging.h"
#include "common.h"
#include "config.h"
#include "util.h"
#include "timezone.h"
#include "binfmt.h"
#include "wslpath.h"
#include "wslinfo.h"
#include "drvfs.h"
#include "plan9.h"
#include "localhost.h"
#include "telemetry.h"
#include "GnsEngine.h"
#include "lxinitshared.h"
#include "message.h"
#include "configfile.h"
#include "CommandLine.h"

static_assert(EX_NOUSER == LX_INIT_USER_NOT_FOUND);
static_assert(EUSERS == LX_INIT_TTY_LIMIT);

#define DEFAULT_SHELL "/bin/sh"
#define DEFAULT_SHELL_ARGS 4
#define HOME_ENV "HOME"
#define LOGNAME_ENV "LOGNAME"
#define SHELL_ENV "SHELL"
#define SHELL_PATH "/bin/sh"
#define USER_ENV "USER"

using namespace wsl::shared;

typedef struct _CREATE_PROCESS_PARSED_COMMON
{
    const char* Filename;
    std::string CurrentWorkingDirectory;
    std::vector<const char*> CommandLine;
    EnvironmentBlock Environment;
    uid_t Uid;
    CREATE_PROCESS_SHELL_OPTIONS ShellOptions;
    bool AllowOOBE;
} CREATE_PROCESS_PARSED_COMMON, *PCREATE_PROCESS_PARSED_COMMON;

typedef struct _CREATE_PROCESS_PARSED
{
    CREATE_PROCESS_PARSED_COMMON Common;
    wil::unique_fd EventFd;
    wil::unique_fd StdFd[LX_INIT_STD_FD_COUNT];
    wil::unique_fd ServiceFd;
} CREATE_PROCESS_PARSED, *PCREATE_PROCESS_PARSED;

struct sigaction g_SavedSignalActions[_NSIG];

//
// Best effort to put all processes launched within a session into the same
// process group. This is how a shell like /bin/bash would typically launch
// grouped commands (e.g. 'find' and 'less' from: find . -iname "*.txt" | less)
//

volatile pid_t g_SessionGroup = -1;

//
// Fallback passwd struct to use in case the /etc/passwd file is missing or
// corrupt.
//

constexpr passwd c_defaultPasswordEntry = {
    const_cast<char*>("root"), NULL, ROOT_UID, ROOT_GID, NULL, const_cast<char*>("/"), const_cast<char*>(DEFAULT_SHELL)};

int CaptureCrash(int Argc, char** Argv);

void CreateProcess(PCREATE_PROCESS_PARSED Parsed, int TtyFd, const wsl::linux::WslDistributionConfig& Config);

void CreateProcessCommon(PCREATE_PROCESS_PARSED_COMMON Common, int TtyFd, int ServiceSocketFd, const wsl::linux::WslDistributionConfig&);

CREATE_PROCESS_PARSED CreateProcessParse(gsl::span<gsl::byte> Buffer, int MessageFd, const wsl::linux::WslDistributionConfig& Config);

int CreateProcessParseCommon(PCREATE_PROCESS_PARSED_COMMON Parsed, gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config);

int CreateProcessReplyToServer(PCREATE_PROCESS_PARSED Parsed, pid_t CreateProcessPid, int MessageFd);

void CreateWslSystemdUnits(const wsl::linux::WslDistributionConfig& Config);

int InitConnectToServer(int LxBusFd, bool WaitForServer);

int InitCreateProcessUtilityVm(
    gsl::span<gsl::byte> Message,
    const LX_INIT_CREATE_PROCESS_UTILITY_VM& Header,
    wsl::shared::SocketChannel& MessageFd,
    const wsl::linux::WslDistributionConfig& Config);

int InitCreateSessionLeader(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, int LxBusFd, wsl::linux::WslDistributionConfig& Config);

void InitEntry(int Argc, char* Argv[]);

void InitEntryWsl(wsl::linux::WslDistributionConfig& Config);

void InitEntryUtilityVm(wsl::linux::WslDistributionConfig& Config);

void InitTerminateInstance(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, wsl::linux::WslDistributionConfig& Config);

void InitTerminateInstanceInternal(const wsl::linux::WslDistributionConfig& Config);

void InstallSystemdUnit(const char* Path, const std::string& Name, const char* Content);

int GenerateSystemdUnits(int Argc, char** Argv);

int GenerateUserSystemdUnits(int Argc, char** Argv);

void HardenMirroredNetworkingSettingsAgainstSystemd();

void PostProcessImportedDistribution(wsl::shared::MessageWriter<LX_MINI_INIT_IMPORT_RESULT>& Message, const char* ExtractedPath);

void SessionLeaderCreateProcess(gsl::span<gsl::byte> Buffer, int MessageFd, int TtyFd);

void SessionLeaderEntry(int MessageFd, int TtyFd, const wsl::linux::WslDistributionConfig& Config);

void SessionLeaderEntryUtilityVm(wsl::shared::SocketChannel& Channel, const wsl::linux::WslDistributionConfig& Config);

unsigned int StartPlan9(int Argc, char** Argv);

unsigned int StartGns(int Argc, char** Argv);

void WaitForBootProcess(wsl::linux::WslDistributionConfig& Config);

wil::unique_fd UnmarshalConsoleFromServer(int MessageFd, LXBUS_IPC_CONSOLE_ID ConsoleId);

int WslEntryPoint(int Argc, char* Argv[])
{
    //
    // Determine if the binary is being launched in init daemon mode by
    // checking the pid and Argc.
    //
    // N.B. Using the pid is not enough because this process might be running
    // in a docker container. See: https://github.com/microsoft/WSL/issues/10883.
    //
    // If not in init daemon mode, differentiate between various functionality by checking Argv[0].
    //

    char* BaseName = basename(Argv[0]);
    int ExitCode = -1;
    pid_t Pid = getpid();

    if (Pid == 1 && strcmp(BaseName, "init") == 0 && Argc <= 1)
    {
        InitEntry(Argc, Argv);
    }
    else
    {
        if (strcmp(BaseName, WSLPATH_NAME) == 0)
        {
            ExitCode = WslPathEntry(Argc, Argv);
        }
        else if (strcmp(BaseName, MOUNT_DRVFS_NAME) == 0)
        {
            ExitCode = MountDrvfsEntry(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_LOCALHOST_RELAY) == 0)
        {
            ExitCode = RunPortTracker(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_TELEMETRY_AGENT) == 0)
        {
            ExitCode = StartTelemetryAgent();
        }
        else if (strcmp(BaseName, LX_INIT_GNS) == 0)
        {
            ExitCode = StartGns(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_PLAN9) == 0)
        {
            ExitCode = StartPlan9(Argc, Argv);
        }
        else if (strcmp(BaseName, WSLINFO_NAME) == 0)
        {
            ExitCode = WslInfoEntry(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_WSL_CAPTURE_CRASH) == 0)
        {
            ExitCode = CaptureCrash(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_WSL_GENERATOR) == 0)
        {
            ExitCode = GenerateSystemdUnits(Argc, Argv);
        }
        else if (strcmp(BaseName, LX_INIT_WSL_USER_GENERATOR) == 0)
        {
            ExitCode = GenerateUserSystemdUnits(Argc, Argv);
        }
        else
        {
            // Handle the special case for import result messages, everything else is sent to the binfmt interpreter.
            if (Pid == 1 && strcmp(BaseName, "init") == 0 && Argc == 3 && strcmp(Argv[1], LX_INIT_IMPORT_MESSAGE_ARG) == 0)
            {
                try
                {
                    wsl::shared::MessageWriter<LX_MINI_INIT_IMPORT_RESULT> message;
                    PostProcessImportedDistribution(message, Argv[2]);
                    UtilWriteBuffer(STDOUT_FILENO, message.Span());
                    char buffer[1];
                    read(STDIN_FILENO, buffer, sizeof(buffer));
                    exit(0);
                }
                CATCH_RETURN_ERRNO()
            }

            ExitCode = CreateNtProcess(Argc - 1, &Argv[1]);
        }
    }

    return ExitCode;
}

int GenerateUserSystemdUnits(int Argc, char** Argv)
{
    if (Argc < 2)
    {
        LOG_ERROR("Unit folder missing");
        return 1;
    }

    const auto* installPath = Argv[1];

    try
    {
        std::string automountRoot = "/mnt";
        wil::unique_file File{fopen("/etc/wsl.conf", "r")};
        if (File)
        {
            std::vector<ConfigKey> ConfigKeys = {
                ConfigKey(wsl::linux::c_ConfigAutoMountRoot, automountRoot),

            };
            ParseConfigFile(ConfigKeys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(CONFIG_FILE));
            File.reset();
        }

        // TODO: handle quotes in path

        auto unitContent = std::format(
            R"(# Note: This file is generated by WSL to configure wslg.

[Unit]
Description=WSLg user service
DefaultDependencies=no

[Service]
Type=oneshot
Environment=WSLG_RUNTIME_DIR={}/{}/{}
ExecStart=/bin/sh -c 'mkdir -p -m 00755 "$XDG_RUNTIME_DIR/pulse"'
ExecStart=/bin/sh -c 'ln -sf "$WSLG_RUNTIME_DIR/wayland-0" "$XDG_RUNTIME_DIR/wayland-0"'
ExecStart=/bin/sh -c 'ln -sf "$WSLG_RUNTIME_DIR/wayland-0.lock" "$XDG_RUNTIME_DIR/wayland-0.lock"'
ExecStart=/bin/sh -c 'ln -sf "$WSLG_RUNTIME_DIR/pulse/native" "$XDG_RUNTIME_DIR/pulse/native"'
ExecStart=/bin/sh -c 'ln -sf "$WSLG_RUNTIME_DIR/pulse/pid" "$XDG_RUNTIME_DIR/pulse/pid"'
  )",
            automountRoot,
            WSLG_SHARED_FOLDER,
            WAYLAND_RUNTIME_DIR);

        InstallSystemdUnit(installPath, "wslg-session", unitContent.c_str());

        return 0;
    }
    CATCH_LOG()

    return 1;
}

int GenerateSystemdUnits(int Argc, char** Argv)
{
    if (Argc < 2)
    {
        LOG_ERROR("Unit folder missing");
        return 1;
    }

    try
    {
        const auto* installPath = Argv[1];

        LOG_INFO("Generating WSL systemd units in {}", installPath);

        bool enableGuiApps = true;
        bool protectBinfmt = true;
        bool interopEnabled = true;
        std::string automountRoot = "/mnt";

        wil::unique_file File{fopen("/etc/wsl.conf", "r")};
        if (File)
        {
            std::vector<ConfigKey> ConfigKeys = {
                ConfigKey(wsl::linux::c_ConfigEnableGuiAppsOption, enableGuiApps),
                ConfigKey(wsl::linux::c_ConfigBootProtectBinfmtOption, protectBinfmt),
                ConfigKey(wsl::linux::c_ConfigInteropEnabledOption, interopEnabled),
                ConfigKey(wsl::linux::c_ConfigAutoMountRoot, automountRoot),

            };
            ParseConfigFile(ConfigKeys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(CONFIG_FILE));
            File.reset();
        }

        // Mask systemd-networkd-wait-online.service since WSL always ensures that networking is configured during boot.
        // That unit can cause systemd boot timeouts since WSL's network interface is unmanaged by systemd.
        THROW_LAST_ERROR_IF(symlink("/dev/null", std::format("{}/systemd-networkd-wait-online.service", installPath).c_str()) < 0);

        // Mask NetworkManager-wait-online.service for the same reason, as it causes timeouts on distros using NetworkManager.
        THROW_LAST_ERROR_IF(symlink("/dev/null", std::format("{}/NetworkManager-wait-online.service", installPath).c_str()) < 0);

        // Only create the wslg unit if both enabled in wsl.conf, and if the wslg folder actually exists.
        if (enableGuiApps && access("/mnt/wslg/runtime-dir", F_OK) == 0)
        {
            THROW_LAST_ERROR_IF(UtilMkdirPath("/run/tmpfiles.d", 0755) < 0);
            const std::string tmpFilesConfig =
                "# Note: This file is generated by WSL to prevent systemd-tmpfiles from removing /tmp/.X11-unix during boot.\n";

            THROW_LAST_ERROR_IF(WriteToFile("/run/tmpfiles.d/x11.conf", tmpFilesConfig.c_str()) < 0);

            // Note: It's not possible to use a mount unit because systemd will not mount /tmp/.X11-unix
            // if /proc/mount says it's already mounted.

            constexpr auto* x11UnitContent = R"(# Note: This file is generated by WSL to prevent tmp.mount from hiding /tmp/.X11-unix

[Unit]
Description=WSLg Remount Service
DefaultDependencies=no
After=systemd-tmpfiles-setup.service tmp.mount
ConditionPathExists=/mnt/wslg/.X11-unix
ConditionPathExists=!/tmp/.X11-unix/X0

[Service]
Type=oneshot
ExecStart=/bin/mount -o bind,ro,X-mount.mkdir -t none /mnt/wslg/.X11-unix /tmp/.X11-unix)";
            InstallSystemdUnit(installPath, "wslg", x11UnitContent);
        }

        if (interopEnabled && protectBinfmt)
        {
            // N.B. ExecStop is required to prevent distributions from removing the WSL binfmt entry on shutdown.
            auto systemdBinfmtContent = std::format(
                R"(# Note: This file is generated by WSL to prevent binfmt.d from overriding WSL's binfmt interpreter.
# To disable this unit, add the following to /etc/wsl.conf:
# [boot]
# protectBinfmt=false

[Service]
ExecStop=
ExecStart=/bin/sh -c '(echo -1 > {}/{}) ; (echo "{}" > {})' )",
                BINFMT_MISC_MOUNT_TARGET,
                LX_INIT_BINFMT_NAME,
                BINFMT_INTEROP_REGISTRATION_STRING(LX_INIT_BINFMT_NAME),
                BINFMT_MISC_REGISTER_FILE);

            // Install the override for systemd-binfmt.service.
            {
                auto overrideFolder = std::format("{}/systemd-binfmt.service.d", installPath);
                THROW_LAST_ERROR_IF(UtilMkdirPath(overrideFolder.c_str(), 0755) < 0);
                THROW_LAST_ERROR_IF(WriteToFile(std::format("{}/override.conf", overrideFolder).c_str(), systemdBinfmtContent.c_str()) < 0);
            }

            // Install the override for binfmt-support.service.
            {
                auto overrideFolder = std::format("{}/binfmt-support.service.d", installPath);
                THROW_LAST_ERROR_IF(UtilMkdirPath(overrideFolder.c_str(), 0755) < 0);
                THROW_LAST_ERROR_IF(WriteToFile(std::format("{}/override.conf", overrideFolder).c_str(), systemdBinfmtContent.c_str()) < 0);
            }
        }

        return 0;
    }
    CATCH_LOG()

    return 1;
}

int CaptureCrash(int Argc, char** Argv)
try
{
    UtilSetThreadName("CaptureCrash");

    if (Argc < 5)
    {
        std::cerr << "Usage: " << Argv[0] << "<time> <executable> <pid> <signal>" << std::endl;
        return 1;
    }

    InitializeLogging(false);

    LOG_INFO("Capturing crash for pid: {}, executable: {}, signal: {}, port: {}", Argv[3], Argv[2], Argv[4], LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);

    wsl::shared::SocketChannel channel(UtilConnectVsock(LX_INIT_UTILITY_VM_CRASH_DUMP_PORT, true), "crash-dump");

    wsl::shared::MessageWriter<LX_PROCESS_CRASH> message(LxProcessCrash);
    message.WriteString(Argv[2]);
    message->Timestamp = std::strtoull(Argv[1], nullptr, 10);
    message->Signal = std::strtoul(Argv[4], nullptr, 10);
    message->Pid = std::strtoull(Argv[3], nullptr, 10);

    auto result = channel.Transaction<LX_PROCESS_CRASH>(message.Span()).Result;
    if (result != 0)
    {
        LOG_ERROR("Received error while trying to capture crash dump: {}", result);
    }

    std::vector<char> buffer(LX_RELAY_BUFFER_SIZE);

    int bytes = -1;
    while ((bytes = TEMP_FAILURE_RETRY(read(STDIN_FILENO, buffer.data(), buffer.size()))) > 0)
    {
        if (UtilWriteBuffer(channel.Socket(), buffer.data(), bytes) < 0)
        {
            LOG_ERROR("Error while trying read write dump, {}", errno);
            return 1;
        }
    }

    if (bytes != 0)
    {
        LOG_ERROR("Error while trying read crash dump from stdin, {}", errno);
        return 1;
    }

    return 0;
}
CATCH_RETURN_ERRNO()

void CreateProcess(PCREATE_PROCESS_PARSED Parsed, int TtyFd, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is the entry point for the create process.

Arguments:

    Parsed - Supplies a pointer to a create process parsed structure.

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    ssize_t BytesRead;
    uint64_t EventFdData;
    int StdFdIndex;

    //
    // Initialize the new process and wait until the session leader signals
    // to execvpe.
    //

    for (StdFdIndex = 0; StdFdIndex < LX_INIT_STD_FD_COUNT; StdFdIndex += 1)
    {
        //
        // If a standard file descriptor is not set, use the TTY file descriptor.
        //

        if (dup2(Parsed->StdFd[StdFdIndex] ? Parsed->StdFd[StdFdIndex].get() : TtyFd, StdFdIndex) < 0)
        {
            FATAL_ERROR("dup2 failed {}", errno);
        }

        Parsed->StdFd[StdFdIndex].reset();
    }

    //
    // Read the eventfd data from the wsl service.
    //

    BytesRead = TEMP_FAILURE_RETRY(read(Parsed->EventFd.get(), &EventFdData, sizeof(EventFdData)));
    if (BytesRead != sizeof(EventFdData))
    {
        FATAL_ERROR("Failed to read (size {}) EventFd {}", BytesRead, errno);
    }

    //
    // Launch the process.
    //

    CreateProcessCommon(&Parsed->Common, TtyFd, Parsed->ServiceFd.get(), Config);
    return;
}

void CreateProcessCommon(PCREATE_PROCESS_PARSED_COMMON Common, int TtyFd, int ServiceSocket, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is entry point for the common create process functionality.

Arguments:

    Common - Supplies a pointer to the common create process parsed data.

    TtyFd - Supplies the file descriptor representing the process's terminal.
        This method takes ownership of this file descriptor.

    ServiceSocket - Supplies the file descriptor to a socket connected to the Service.

    Config - Supplies the distribution configuration

Return Value:

    None.

--*/

try
{
    //
    // Print any errors that occurred.
    //

    for (const auto& e : wsl::shared::string::Split<char>(wil::ScopedWarningsCollector::ConsumeWarnings(), '\n'))
    {
        if (!e.empty())
        {
            fprintf(stderr, "wsl: %s\n", e.c_str());
        }
    }

    //
    // Restore default signal dispositions and clear the signal mask for the child process.
    //

    THROW_LAST_ERROR_IF(UtilSetSignalHandlers(g_SavedSignalActions, false) < 0);

    sigset_t SignalMask;
    sigemptyset(&SignalMask);
    THROW_LAST_ERROR_IF(sigprocmask(SIG_SETMASK, &SignalMask, NULL) < 0);

    auto AddEnvironmentVariable = [&](const char* Name) {
        const auto Value = UtilGetEnvironmentVariable(Name);
        if (!Value.empty())
        {
            Common->Environment.AddVariable(Name, Value.c_str());
        }
    };

    AddEnvironmentVariable(NAME_ENV);
    AddEnvironmentVariable(WSL_DISTRO_NAME_ENV);

    //
    // Get the password entry for the user. (root if the distribution is being installed)
    //

    const passwd* PasswordEntry{};

    auto ConfigureUid = [&](uint32_t Uid) {
        PasswordEntry = getpwuid(Uid);
        if (PasswordEntry == nullptr)
        {
            LOG_ERROR("getpwuid({}) failed {}", Uid, errno);
            PasswordEntry = const_cast<passwd*>(&c_defaultPasswordEntry);
        }

        //
        // Add environment variables to the environment block.
        //

        Common->Environment.AddVariable(HOME_ENV, PasswordEntry->pw_dir);
        Common->Environment.AddVariable(USER_ENV, PasswordEntry->pw_name);
        Common->Environment.AddVariable(LOGNAME_ENV, PasswordEntry->pw_name);
        Common->Environment.AddVariable(SHELL_ENV, PasswordEntry->pw_shell);
    };

    //
    // Set the $LANG environment variable.
    //
    // N.B. Failure to update $LANG environment variable is non-fatal.
    //

    ConfigUpdateLanguage(Common->Environment);

    //
    // Launch the OOBE command, if any
    //

    if (Common->AllowOOBE)
    {
        assert(ServiceSocket != -1);

        wsl::shared::SocketChannel channel(wil::unique_fd{ServiceSocket}, "OOBE");

        std::string OobeCommand{};
        int defaultUid = 0;
        ConfigKeyPresence defaultUidPresent{};
        std::vector<ConfigKey> keys = {ConfigKey("oobe.command", OobeCommand), ConfigKey("oobe.defaultUid", defaultUid, &defaultUidPresent)};

        {
            wil::unique_file File{fopen(WSL_DISTRIBUTION_CONF, "r")};
            ParseConfigFile(keys, File.get(), CFG_SKIP_UNKNOWN_VALUES, STRING_TO_WSTRING(CONFIG_FILE));
        }

        int32_t OobeResult = 0;
        if (!OobeCommand.empty())
        {
            auto Pid = UtilCreateChildProcess("OOBE", [&OobeCommand, &Common, &ConfigureUid]() {
                ConfigureUid(0);
                execle("/bin/sh", "sh", "-c", OobeCommand.c_str(), nullptr, const_cast<char**>(Common->Environment.Variables().data()));
                LOG_ERROR("execle() failed, {}", errno);
            });

            int Status = -1;
            if (TEMP_FAILURE_RETRY(waitpid(Pid, &Status, 0)) < 0)
            {
                LOG_ERROR("Waiting for child '{}' failed, waitpid failed {}", OobeCommand.c_str(), errno);
                _exit(1);
            }

            if (UtilProcessChildExitCode(Status, OobeCommand.c_str(), 0, false) < 0)
            {
                OobeResult = -1;
                fprintf(stderr, "OOBE command \"%s\" failed, exiting\n", OobeCommand.c_str());
            }
        }

        LX_INIT_OOBE_RESULT result{};
        result.Header.MessageType = LxInitOobeResult;
        result.Header.MessageSize = sizeof(result);
        result.Result = OobeResult;
        result.DefaultUid = defaultUidPresent == ConfigKeyPresence::Present ? defaultUid : -1;

        channel.SendMessage(result);

        if (OobeResult != 0)
        {
            _exit(1);
        }

        ConfigureUid(defaultUidPresent == ConfigKeyPresence::Present ? defaultUid : Common->Uid);
    }
    else
    {
        ConfigureUid(Common->Uid);
    }

    //
    // Ensure that a login session has been created for the user and set expected
    // environment variables.
    //

    if (Config.InitPid.has_value())
    {
        wsl::shared::SocketChannel InteropChannel{UtilConnectToInteropServer(Config.InitPid.value()), "InteropClient"};
        THROW_LAST_ERROR_IF(InteropChannel.Socket() < 0);

        wsl::shared::MessageWriter<LX_INIT_CREATE_LOGIN_SESSION> CreateSession(LxInitMessageCreateLoginSession);
        CreateSession->Uid = PasswordEntry->pw_uid;
        CreateSession->Gid = PasswordEntry->pw_gid;
        CreateSession.WriteString(PasswordEntry->pw_name);

        auto result = InteropChannel.Transaction<LX_INIT_CREATE_LOGIN_SESSION>(CreateSession.Span());

        if (!result.Result)
        {
            fprintf(stderr, "wsl: %s\n", wsl::shared::Localization::MessageSystemdUserSessionFailed(PasswordEntry->pw_name).c_str());
        }

        Common->Environment.AddVariable("DBUS_SESSION_BUS_ADDRESS", std::format("unix:path=/run/user/{}/bus", PasswordEntry->pw_uid));
        Common->Environment.AddVariable(XDG_RUNTIME_DIR_ENV, std::format("/run/user/{}", PasswordEntry->pw_uid));
    }

    //
    // If a filename was provided, use the filename and command line as-is.
    // Otherwise, use the user's default shell. If the user's default shell is
    // is empty, fall back to using /bin/sh.
    //

    std::string Argv0;
    std::vector<const char*> CommandLine = Common->CommandLine;
    char* Filename = const_cast<char*>(Common->Filename);
    if (strlen(Filename) == 0)
    {
        Filename = const_cast<char*>(SHELL_PATH);
        auto Size = sizeof(SHELL_PATH) - 1;
        if (PasswordEntry->pw_shell != NULL)
        {
            Size = strlen(PasswordEntry->pw_shell);
            if (Size != 0)
            {
                Filename = PasswordEntry->pw_shell;
            }
        }

        if ((Common->ShellOptions & ShellOptionsLogin) != 0)
        {
            //
            // Construct the name of the shell as the last path element
            // prepended with a '-' and use this as Argv[0].
            //
            // N.B. This is the same behavior as the login binary.
            //

            auto Shell = strrchr(Filename, '/');
            if (Shell != nullptr)
            {
                Shell = Shell + 1;
            }
            else
            {
                Shell = Filename;
            }

            Argv0 = "-";
            Argv0 += Shell;
        }
        else
        {
            Argv0 = Filename;
        }

        CommandLine.insert(CommandLine.begin(), Argv0.c_str());
    }

    //
    // Set the owner of the tty device.
    //

    if (TtyFd != -1)
    {
        if (fchown(TtyFd, PasswordEntry->pw_uid, TTY_GID) < 0)
        {
            LOG_ERROR("fchown failed {}", errno);
        }

        CLOSE(TtyFd);
        TtyFd = -1;
    }

    //
    // Set the supplemental groups, gid, uid, and current working directory.
    //

    UtilInitGroups(PasswordEntry->pw_name, PasswordEntry->pw_gid);
    THROW_LAST_ERROR_IF(setgid(PasswordEntry->pw_gid) < 0);
    THROW_LAST_ERROR_IF(setuid(PasswordEntry->pw_uid) < 0);

    //
    // If the provided current working directory is empty, use the user's home
    // path as the current working directory.
    //
    // N.B. Failures to set the current working directory are non-fatal.
    //

    std::string Directory;
    if (Common->CurrentWorkingDirectory.empty())
    {
        Directory = PasswordEntry->pw_dir;
    }
    else
    {
        Directory = Common->CurrentWorkingDirectory;
        if (Directory[0] == '~')
        {
            Directory = PasswordEntry->pw_dir;
            if (Common->CurrentWorkingDirectory.size() > 1)
            {
                Directory += &Common->CurrentWorkingDirectory[1];
            }
        }
    }

    if (chdir(Directory.c_str()) < 0)
    {
        LOG_ERROR("chdir({}) failed {}", Directory, errno);
    }

    //
    // Launch the process.
    //

    execvpe(Filename, const_cast<char**>(CommandLine.data()), const_cast<char**>(Common->Environment.Variables().data()));
    FATAL_ERROR("execvpe({}) failed: {}", Filename, strerror(errno));

    return;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    FATAL_ERROR("Create process failed");
}

CREATE_PROCESS_PARSED CreateProcessParse(gsl::span<gsl::byte> Buffer, int MessageFd, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine parses a create process message.

Arguments:

    Buffer - Supplies the create process message.

    MessageFd - Supplies a message port file descriptor.

    Config - Supplies the distribution configuration.

Return Value:

    The create process parameters.

--*/

{
    //
    // Validate the message size.
    //

    auto* Message = gslhelpers::try_get_struct<LX_INIT_CREATE_PROCESS>(Buffer);
    THROW_ERRNO_IF(EINVAL, !Message);

    //
    // Parse common create process information.
    //

    CREATE_PROCESS_PARSED Parsed{};
    int Result = CreateProcessParseCommon(&Parsed.Common, Buffer.subspan(offsetof(LX_INIT_CREATE_PROCESS, Common)), Config);
    THROW_ERRNO_IF(EINVAL, Result < 0);

    //
    // Create the eventfd.
    //

    Parsed.EventFd = eventfd(0, EFD_CLOEXEC);
    THROW_LAST_ERROR_IF(!Parsed.EventFd);

    //
    // Set up the standard handles for the process.
    //
    //

    for (unsigned short Index = 0; Index < LX_INIT_STD_FD_COUNT; Index += 1)
    {
        if (Message->StdFdIds[Index] != LX_INIT_CREATE_PROCESS_USE_CONSOLE)
        {
            LXBUS_IPC_MESSAGE_UNMARSHAL_HANDLE_PARAMETERS UnmarshalHandle{};
            UnmarshalHandle.Input.HandleId = Message->StdFdIds[Index];
            Result = TEMP_FAILURE_RETRY(ioctl(MessageFd, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_HANDLE, &UnmarshalHandle));
            THROW_LAST_ERROR_IF(Result < 0);

            Parsed.StdFd[Index] = UnmarshalHandle.Output.FileDescriptor;
        }
    }

    //
    // Unmarshal the fork token.
    //

    LXBUS_IPC_MESSAGE_UNMARSHAL_FORK_TOKEN_PARAMETERS UnmarshalForkToken{};
    UnmarshalForkToken.Input.ForkTokenId = Message->ForkTokenId;
    Result = TEMP_FAILURE_RETRY(ioctl(MessageFd, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_FORK_TOKEN, &UnmarshalForkToken));
    THROW_LAST_ERROR_IF(Result < 0);

    //
    // Unmarshal the ipc server.
    //

    if (Message->IpcServerId != LXBUS_IPC_SERVER_ID_INVALID)
    {
        LXBUS_IPC_MESSAGE_UNMARSHAL_SERVER_PARAMETERS UnmarshalServer{};
        UnmarshalServer.Input.ServerId = Message->IpcServerId;
        Result = TEMP_FAILURE_RETRY(ioctl(MessageFd, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_SERVER, &UnmarshalServer));
        THROW_LAST_ERROR_IF(Result < 0);

        if (Parsed.Common.AllowOOBE)
        {
            wil::unique_fd LxBusFd{TEMP_FAILURE_RETRY(open(LXBUS_DEVICE_NAME, O_RDWR))};
            THROW_LAST_ERROR_IF(!LxBusFd);

            LXBUS_CONNECT_SERVER_PARAMETERS ConnectParams{};
            ConnectParams.Input.Flags = LXBUS_IPC_CONNECT_FLAG_UNNAMED_SERVER;
            ConnectParams.Input.TimeoutMs = LXBUS_IPC_INFINITE_TIMEOUT;
            Result = TEMP_FAILURE_RETRY(ioctl(LxBusFd.get(), LXBUS_IOCTL_CONNECT_SERVER, &ConnectParams));
            THROW_LAST_ERROR_IF(Result < 0);

            Parsed.ServiceFd = ConnectParams.Output.MessagePort;
        }
    }

    return Parsed;
}

int CreateProcessParseCommon(PCREATE_PROCESS_PARSED_COMMON Parsed, gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine parses a create process message.

Arguments:

    Parsed - Supplies a buffer to store the common create process parameters.

    Buffer - Supplies the common create process message data.

    Config - Supplies the distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    auto* Common = gslhelpers::try_get_struct<LX_INIT_CREATE_PROCESS_COMMON>(Buffer);
    if (!Common)
    {
        LOG_ERROR("Invalid message size {}", Buffer.size());
        return -1;
    }

    //
    // Populate the current working directory. If the path does not begin with a
    // UNIX path separator or `~`, it is translated.
    //
    // N.B. Failure to translate the current working directory is non-fatal.
    //

    auto* Path = wsl::shared::string::FromSpan(Buffer, Common->CurrentWorkingDirectoryOffset);
    if ((*Path == '/') || (*Path == '~'))
    {
        Parsed->CurrentWorkingDirectory = Path;
    }
    else if (*Path != '\0')
    {
        Parsed->CurrentWorkingDirectory = WslPathTranslate(const_cast<char*>(Path), TRANSLATE_FLAG_ABSOLUTE, TRANSLATE_MODE_UNIX);
        if (Parsed->CurrentWorkingDirectory.empty() && Config.AutoMount)
        {
            EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToTranslate(Path));
        }
    }

    //
    // Initialize the command line will a null-terminator.
    //

    auto CommandLine = Buffer.subspan(Common->CommandLineOffset);
    for (unsigned short Index = 0; Index < Common->CommandLineCount; Index += 1)
    {
        std::string_view Argument{wsl::shared::string::FromSpan(CommandLine)};
        Parsed->CommandLine.emplace_back(Argument.data());
        CommandLine = CommandLine.subspan(Argument.size() + 1);
    }

    //
    // If a username was provided, get the password entry for the specified username.
    // If no username was provided use the one specified in /etc/wsl.conf.
    // Otherwise, use the default UID from the registry.
    //

    struct passwd* PasswordEntry = nullptr;
    auto Username = wsl::shared::string::FromSpan(Buffer, Common->UsernameOffset);
    if (strlen(Username) != 0)
    {
        PasswordEntry = getpwnam(Username);
        if (PasswordEntry == nullptr)
        {
            FATAL_ERROR_EX(EX_NOUSER, "getpwnam({}) failed {}", Username, errno);
        }
    }
    else if (Config.DefaultUser.has_value())
    {
        PasswordEntry = getpwnam(Config.DefaultUser->c_str());
        if (PasswordEntry == nullptr)
        {
            LOG_ERROR("getpwnam({}) failed {}", Config.DefaultUser->c_str(), errno);
        }
    }

    if (PasswordEntry == nullptr)
    {
        PasswordEntry = getpwuid(Common->DefaultUid);
        if (PasswordEntry == nullptr)
        {
            LOG_ERROR("getpwuid({}) failed {}", Common->DefaultUid, errno);
        }
    }

    Parsed->CommandLine.emplace_back(nullptr);
    Parsed->Environment = ConfigCreateEnvironmentBlock(Common, Config);
    Parsed->Filename = wsl::shared::string::FromSpan(Buffer, Common->FilenameOffset);
    Parsed->ShellOptions = static_cast<CREATE_PROCESS_SHELL_OPTIONS>(Common->ShellOptions);
    Parsed->Uid = PasswordEntry ? PasswordEntry->pw_uid : ROOT_UID; // If the default user was not found, fall back to root.
    Parsed->AllowOOBE = WI_IsFlagSet(Common->Flags, LxInitCreateProcessFlagAllowOOBE);
    return 0;
}
CATCH_RETURN_ERRNO()

int CreateProcessReplyToServer(PCREATE_PROCESS_PARSED Parsed, pid_t CreateProcessPid, int MessageFd)

/*++

Routine Description:

    This routine replies to the server for a create process message.

Arguments:

    Parsed - Supplies a pointer to a create process parsed structure.

    CreateProcessPid - Supplies the pid of a newly created child process.

    MessageFd - Supplies a message port file descriptor.

Return Value:

    0 on success, -1 on failure.

    N.B. On failure, this routine will terminate the child process.

--*/

{
    auto terminateChild = wil::scope_exit([CreateProcessPid]() {
        if (kill(CreateProcessPid, SIGKILL) < 0)
        {
            FATAL_ERROR("Failed to kill child process {}", errno);
        }
    });

    //
    // Marshal the pid of the new child process and send a message
    // indicating that the child was created.
    //

    LXBUS_IPC_MESSAGE_MARSHAL_PROCESS_PARAMETERS MarshalProcess{};
    MarshalProcess.Input.Process = CreateProcessPid;
    if (TEMP_FAILURE_RETRY(ioctl(MessageFd, LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_PROCESS, &MarshalProcess)) < 0)
    {
        LOG_ERROR("Failed to marshal pid {}", errno);
        return -1;
    }

    auto Bytes = UtilWriteBuffer(MessageFd, &MarshalProcess.Output.ProcessId, sizeof(MarshalProcess.Output.ProcessId));
    if (Bytes < 0)
    {
        LOG_ERROR("Failed to write ProcessId {}", errno);
        return -1;
    }

    //
    // Wait for the server to indicate that the process can be continued or
    // it needs to be terminated.
    //

    Bytes = TEMP_FAILURE_RETRY(read(MessageFd, &MarshalProcess.Output.ProcessId, sizeof(MarshalProcess.Output.ProcessId)));
    if (Bytes != sizeof(MarshalProcess.Output.ProcessId))
    {
        LOG_ERROR("Failed to read (size {}) ProcessId {}", Bytes, errno);
        return -1;
    }

    if (MarshalProcess.Output.ProcessId == 0)
    {
        LOG_ERROR("Server replied with failure");
        return -1;
    }

    uint64_t EventFdData = 1;
    Bytes = UtilWriteBuffer(Parsed->EventFd.get(), &EventFdData, sizeof(EventFdData));
    if (Bytes < 0)
    {
        LOG_ERROR("Failed to write EventFd {}", errno);
        return -1;
    }

    terminateChild.release();
    return 0;
}

int InitCreateSessionLeader(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, int LxBusFd, wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine creates a session leader from the init process.

Arguments:

    Buffer - Supplies the message buffer.

    Channel - Supplies a message channel

    LxBusFd - Supplies an LxBus file descriptor (WSL1 only).

    Config - Supplies the distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/
try
{
    int Result = -1;
    pid_t SessionLeader = -1;
    wil::unique_fd SessionLeaderFd;
    struct sockaddr_vm SocketAddress;

    //
    // N.B. FATAL_ERROR will exit the init process, which will also terminate
    //      any previously created sessions that are still running. On failure,
    //      the calling function may choose to continue, in order to preserve
    //      these previous sessions. The FATAL_ERROR macro should be used with
    //      care here.
    //

    //
    // Validate input parameters.
    //

    auto* CreateSession = gslhelpers::try_get_struct<LX_INIT_CREATE_SESSION>(Buffer);
    if (!CreateSession)
    {
        FATAL_ERROR("Unexpected create session size {}", Buffer.size());
    }

    //
    // Connect to the Windows server for the new session leader and create the
    // new session leader process.
    //

    if (LxBusFd >= 0)
    {
        //
        // Unmarshal the console for the session leader.
        //

        if (CreateSession->ConsoleId == LX_INIT_NO_CONSOLE)
        {
            FATAL_ERROR("Console required for session leader");
        }

        auto TtyFd = UnmarshalConsoleFromServer(Channel.Socket(), CreateSession->ConsoleId);
        if (!TtyFd)
        {
            Result = -1;
            LOG_ERROR("UnmarshalConsoleFromServer failed");
            goto InitCreateSessionLeaderExit;
        }

        SessionLeaderFd = InitConnectToServer(LxBusFd, false);
        if (!SessionLeaderFd)
        {
            Result = -1;
            goto InitCreateSessionLeaderExit;
        }

        SessionLeader = UtilCreateChildProcess(
            "SessionLeader", [SessionLeaderFd = std::move(SessionLeaderFd), TtyFd = std::move(TtyFd), &Channel, &Config]() mutable {
                umask(Config.Umask);
                Channel.Close();

                THROW_LAST_ERROR_IF(UtilRestoreBlockedSignals() < 0);

                SessionLeaderEntry(SessionLeaderFd.get(), TtyFd.get(), Config);
            });
    }
    else
    {
        WaitForBootProcess(Config);

        //
        // Ensure the /etc/resolv.conf symlink is present.
        //

        ConfigCreateResolvConfSymlink(Config);

        //
        // Create a listening socket for the service to connect to and tell the
        // service which port to use.
        //
        // N.B. If creating the socket fails, a message with invalid port number
        //      should be sent to unblock the wsl service.
        //

        wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, 1)};
        if (!ListenSocket)
        {
            SocketAddress.svm_port = -1;
        }

        LX_INIT_CREATE_SESSION_RESPONSE Response;
        Response.Header.MessageType = LxInitMessageCreateSessionResponse;
        Response.Header.MessageSize = sizeof(Response);
        Response.Port = SocketAddress.svm_port;
        Channel.SendMessage(Response);

        if (!ListenSocket)
        {
            Result = -1;
            goto InitCreateSessionLeaderExit;
        }

        // Note: The call to accept() must be done in the child because if accept() takes a long time, it can block the creation
        // of other session leaders. See https://github.com/microsoft/WSL/issues/9114.

        SessionLeader = UtilCreateChildProcess(
            "SessionLeader", [ListenSocket = std::move(ListenSocket), &Channel, &Config, Mask = Config.Umask, SocketAddress]() {
                umask(Mask);
                Channel.Close();

                THROW_LAST_ERROR_IF(UtilRestoreBlockedSignals() < 0);

                wsl::shared::SocketChannel channel{
                    {UtilAcceptVsock(ListenSocket.get(), SocketAddress, SESSION_LEADER_ACCEPT_TIMEOUT_MS)}, "SessionLeader"};
                if (channel.Socket() < 0)
                {
                    LOG_ERROR("UtilAcceptVsock() failed for session leader {}", errno);
                    _exit(1);
                }

                SessionLeaderEntryUtilityVm(channel, Config);
            });
    }

    if (SessionLeader < 0)
    {
        Result = -1;
        goto InitCreateSessionLeaderExit;
    }

    Result = 0;

InitCreateSessionLeaderExit:
    return Result;
}
CATCH_RETURN_ERRNO();

int InitConnectToServer(int LxBusFd, bool WaitForServer)

/*++

Routine Description:

    This routine connects the init process to the lxbus server.

Arguments:

    LxBusFd - Supplies an LxBus file descriptor.

    WaitForServer - Supplies true to wait for the server, false otherwise.

Return Value:

    A message port file descriptor on success, -1 on failure.

--*/

{
    LXBUS_CONNECT_SERVER_PARAMETERS Connection;
    int MessageFd;
    int Result;

    MessageFd = -1;
    memset(&Connection, 0, sizeof(Connection));

    //
    // Connect to the server and set the CLOEXEC flag.
    //

    Connection.Input.ServerName = LX_INIT_SERVER_NAME;
    Connection.Input.TimeoutMs = LXBUS_IPC_INFINITE_TIMEOUT;
    if (WaitForServer != false)
    {
        Connection.Input.Flags = LXBUS_IPC_CONNECT_FLAG_WAIT_FOR_SERVER_REGISTRATION;
    }

    Result = TEMP_FAILURE_RETRY(ioctl(LxBusFd, LXBUS_IOCTL_CONNECT_SERVER, &Connection));
    if (Result < 0)
    {
        FATAL_ERROR("Failed to connect to server {}", errno);
    }

    MessageFd = Connection.Output.MessagePort;
    Result = fcntl(MessageFd, F_SETFD, FD_CLOEXEC);
    if (Result < 0)
    {
        FATAL_ERROR("fcntl failed {}", errno);
    }

    return MessageFd;
}

int InitCreateProcessUtilityVm(
    gsl::span<gsl::byte> Span,
    const LX_INIT_CREATE_PROCESS_UTILITY_VM& CreateProcess,
    wsl::shared::SocketChannel& Channel,
    const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine creates a process from init.

Arguments:

    Span - Supplies the message buffer.

    CreateProcess - Supplies the message

    Channel - Supplies the channel.

    Config - Supplies the distribution configuration.

Return Value:

    0 on success, -1 on failure.

--*/

{
    std::vector<gsl::byte> Buffer;
    ssize_t BytesRead;
    ssize_t BytesWritten;
    pid_t ChildPid;
    wsl::shared::SocketChannel ControlChannel;

    LX_INIT_PROCESS_EXIT_STATUS ExitStatus;
    unsigned int Index;
    bool InteropEnabled;
    InteropServer InteropServer;
    int ListenSocket = -1;
    int Master = -1;
    CREATE_PROCESS_PARSED_COMMON Parsed = {nullptr};
    std::vector<gsl::byte> PendingStdin;
    struct pollfd PollDescriptors[7];
    pid_t RelayPid = -1;
    int Result;
    int SignalFd = -1;
    struct signalfd_siginfo SignalInfo;
    sigset_t SignalMask;
    struct sockaddr_vm SocketAddress;
    std::vector<wil::unique_fd> Sockets(LX_INIT_UTILITY_VM_CREATE_PROCESS_SOCKET_COUNT);
    int Status;
    wil::unique_pipe StdErrPipe;
    int StdIn = -1;
    wil::unique_pipe StdInPipe;
    wil::unique_pipe StdOutPipe;
    wsl::shared::SocketChannel TerminalControlChannel;

    int TtyFd = -1;
    struct winsize WindowSize;

    //
    // Connect an extra socket for OOBE, if requested.
    //

    if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagAllowOOBE))
    {
        Sockets.push_back(wil::unique_fd{});
    }

    //
    // Create a listening socket to accept connections for stdin, stdout,
    // stderr, and the control channel.
    //
    // N.B. If creating the socket fails, a message with invalid port number
    //      should be sent to unblock the wsl service.
    //

    ListenSocket = UtilListenVsockAnyPort(&SocketAddress, Sockets.size());
    if (ListenSocket < 0)
    {
        SocketAddress.svm_port = -1;
    }

    //
    // Tell the service which sockets ports to connect to.
    //

    Channel.SendResultMessage<uint32_t>(SocketAddress.svm_port);

    //
    // Exit if creating the listening socket failed.
    //

    if (ListenSocket < 0)
    {
        Result = -1;
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Create a process to relay input and output via sockets. The parent
    // returns to continue processing messages.
    //

    RelayPid = fork();
    if (RelayPid < 0)
    {
        FATAL_ERROR("fork failed for child process {}", errno);
    }

    if (RelayPid > 0)
    {
        Result = 0;
        goto CreateProcessUtilityVmEnd;
    }

    UtilSetThreadName("Relay");

    //
    // Move to the correct mount namespace to create the child in.
    //

    if (ConfigSetMountNamespace(WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsElevated)) < 0)
    {
        Result = -1;
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Accept connections from the wsl service.
    //

    for (auto& Socket : Sockets)
    {
        Socket.reset(UtilAcceptVsock(ListenSocket, SocketAddress));
        if (Socket.get() < 0)
        {
            Result = -1;
            goto CreateProcessUtilityVmEnd;
        }
    }

    //
    // Close the listening socket.
    //

    CLOSE(ListenSocket);
    ListenSocket = -1;

    //
    // Initialize interop.
    //

    InteropEnabled = WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsInteropEnabled) && Config.InteropEnabled;
    if (InteropEnabled)
    {
        Result = InteropServer.Create();
        if (Result < 0)
        {
            goto CreateProcessUtilityVmEnd;
        }
    }

    //
    // For any of the standard handles that are not consoles, create pipes.
    //

    if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdInConsole))
    {
        try
        {
            StdInPipe = wil::unique_pipe::create(O_CLOEXEC);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION_MSG("pipe failed");
            Result = -1;
            goto CreateProcessUtilityVmEnd;
        }
    }

    if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdOutConsole))
    {
        try
        {
            StdOutPipe = wil::unique_pipe::create(O_CLOEXEC);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION_MSG("pipe failed");
            Result = -1;
            goto CreateProcessUtilityVmEnd;
        }
    }

    if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdErrConsole))
    {
        try
        {
            StdErrPipe = wil::unique_pipe::create(O_CLOEXEC);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION_MSG("pipe failed");
            Result = -1;
            goto CreateProcessUtilityVmEnd;
        }
    }

    //
    // Mark the relay process as a subreaper.
    //

    Result = prctl(PR_SET_CHILD_SUBREAPER, 1);
    if (Result < 0)
    {
        LOG_ERROR("prctl failed {}", errno);
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Block SIGCHLD.
    // N.B. This needs to be done before forking so SIGCHILD isn't missed
    // in case the child exits before the relay masks the signal.
    //

    sigemptyset(&SignalMask);
    sigaddset(&SignalMask, SIGCHLD);
    Result = sigprocmask(SIG_BLOCK, &SignalMask, nullptr);
    if (Result < 0)
    {
        LOG_ERROR("sigprocmask failed {}", errno);
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Create a pseudoterminal and child process.
    //

    memset(&WindowSize, 0, sizeof(WindowSize));
    WindowSize.ws_col = CreateProcess.Columns;
    WindowSize.ws_row = CreateProcess.Rows;
    Result = forkpty(&Master, NULL, NULL, &WindowSize);
    if (Result < 0)
    {
        LOG_ERROR("forkpty failed {}", errno);
        goto CreateProcessUtilityVmEnd;
    }

    if (Result == 0)
    {
        //
        // Reset the signal masks.
        //

        sigemptyset(&SignalMask);
        Result = sigprocmask(SIG_SETMASK, &SignalMask, nullptr);
        if (Result < 0)
        {
            LOG_ERROR("sigprocmask failed {}", errno);
            goto CreateProcessUtilityVmEnd;
        }

        //
        // Duplicate stdin to get a file descriptor representing the controlling
        // terminal.
        //

        TtyFd = dup(STDIN_FILENO);
        if (TtyFd < 0)
        {
            LOG_ERROR("dup failed {}", errno);
            goto CreateProcessUtilityVmEnd;
        }

        //
        // Replace any standard file descriptor that is not a console with a
        // pipe.
        //

        if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdInConsole))
        {
            Result = dup2(StdInPipe.read().get(), STDIN_FILENO);
            if (Result < 0)
            {
                LOG_ERROR("dup2 failed {}", errno);
                goto CreateProcessUtilityVmEnd;
            }
        }

        if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdOutConsole))
        {
            Result = dup2(StdOutPipe.write().get(), STDOUT_FILENO);
            if (Result < 0)
            {
                LOG_ERROR("dup2 failed {}", errno);
                goto CreateProcessUtilityVmEnd;
            }
        }

        if (WI_IsFlagClear(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdErrConsole))
        {
            Result = dup2(StdErrPipe.write().get(), STDERR_FILENO);
            if (Result < 0)
            {
                LOG_ERROR("dup2 failed {}", errno);
                goto CreateProcessUtilityVmEnd;
            }
        }

        //
        // Parse common create process information and create the process in
        // the child.
        //

        Result = CreateProcessParseCommon(&Parsed, Span.subspan(offsetof(LX_INIT_CREATE_PROCESS_UTILITY_VM, Common)), Config);
        if (Result < 0)
        {
            goto CreateProcessUtilityVmEnd;
        }

        //
        // Set the unique interop socket name as an environment variable.
        //

        if (InteropEnabled)
        {
            Result = Parsed.Environment.AddVariableNoThrow(WSL_INTEROP_ENV, InteropServer.Path());
            if (Result < 0)
            {
                goto CreateProcessUtilityVmEnd;
            }
        }

        CreateProcessCommon(&Parsed, TtyFd, Sockets.size() >= 6 ? Sockets[5].get() : -1, Config);
        TtyFd = -1;
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Parent...
    //

    ChildPid = Result;

    if (Sockets.size() >= 6)
    {
        Sockets[5].reset();
    }

    //
    // Add the child pid to the thread name for convenience.
    //

    UtilSetThreadName(std::format("Relay({})", ChildPid).c_str());

    //
    // Close the unneeded ends of the std pipes.
    //

    StdInPipe.read().reset();
    StdOutPipe.write().reset();
    StdErrPipe.write().reset();

    //
    // Create a signalfd to detect when the child process exits.
    //

    SignalFd = signalfd(-1, &SignalMask, 0);
    if (SignalFd < 0)
    {
        Result = -1;
        LOG_ERROR("signalfd failed {}", errno);
        goto CreateProcessUtilityVmEnd;
    }

    //
    // Duplicate the stdin file descriptor.
    //

    if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdInConsole))
    {
        StdIn = dup(Master);
    }
    else
    {
        StdIn = dup(StdInPipe.write().get());
        StdInPipe.write().reset();
    }

    if (StdIn < 0)
    {
        Result = -1;
        LOG_ERROR("dup failed {}", errno);
        goto CreateProcessUtilityVmEnd;
    }

    THROW_LAST_ERROR_IF(fcntl(StdIn, F_SETFL, O_NONBLOCK) < 0);

    //
    // Fill the poll descriptors.
    //
    // N.B. Any files descriptors that are -1 are ignored by poll.
    //

    PollDescriptors[0].fd = Sockets[0].get();
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[1].fd = StdOutPipe.read().get();
    PollDescriptors[1].events = POLLIN;
    PollDescriptors[2].fd = StdErrPipe.read().get();
    PollDescriptors[2].events = POLLIN;
    PollDescriptors[3].fd = Master;
    PollDescriptors[3].events = POLLIN;
    PollDescriptors[4].fd = InteropServer.Socket();
    PollDescriptors[4].events = POLLIN;
    PollDescriptors[5].fd = SignalFd;
    PollDescriptors[5].events = POLLIN;
    PollDescriptors[6].fd = Sockets[3].get();
    PollDescriptors[6].events = POLLIN;

    TerminalControlChannel = {{Sockets[3].get()}, "TerminalControl"};

    //
    // This is required because sequence numbers can be reset during handover from wsl.exe to wslhost.exe.
    //

    TerminalControlChannel.IgnoreSequenceNumbers();

    ControlChannel = {{Sockets[4].get()}, "Control"};

    //
    // Begin relaying data from the stdin socket to stdin file descriptor and
    // from the master PTY endpoint and output pipes to the stdout and stderr
    // sockets.
    //

    for (;;)
    {
        BytesWritten = 0;

        Result = poll(PollDescriptors, COUNT_OF(PollDescriptors), PendingStdin.empty() ? -1 : 100);
        if (!PendingStdin.empty())
        {
            BytesWritten = write(StdIn, PendingStdin.data(), PendingStdin.size());
            if (BytesWritten < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    LOG_ERROR("delayed stdin write failed {}, ChildPid={}", errno, ChildPid);
                }
            }
            else if (BytesWritten <= PendingStdin.size()) // Partial or complete write
            {
                PendingStdin.erase(PendingStdin.begin(), PendingStdin.begin() + BytesWritten);
            }
            else
            {
                LOG_ERROR("Unexpected write result {}, pending={}", BytesWritten, PendingStdin.size());
            }
        }

        if (Result < 0)
        {
            LOG_ERROR("poll failed {}", errno);
            break;
        }

        //
        // Relay input from the stdin socket to the stdin file descriptor.
        //

        if (PollDescriptors[0].revents & (POLLIN | POLLHUP | POLLERR) && PendingStdin.empty())
        {
            BytesRead = UtilReadBuffer(Sockets[0].get(), Buffer);
            if (BytesRead < 0)
            {
                LOG_ERROR("read failed {}", errno);
                break;
            }

            //
            // A zero-byte read means that the stdin socket has closed. Close
            // the corresponding stdin file descriptor and remove the stdin
            // socket from the poll descriptors list.
            //

            if (BytesRead == 0)
            {
                CLOSE(StdIn);
                StdIn = -1;
                PollDescriptors[0].fd = -1;

                //
                // If stdin is a console, close the pseudoterminal master.
                //

                if ((WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdInConsole)) && (Master != -1))
                {
                    CLOSE(Master);
                    Master = -1;
                    PollDescriptors[3].fd = -1;
                }
            }
            else
            {
                BytesWritten = write(StdIn, Buffer.data(), BytesRead);
                if (BytesWritten < 0)
                {
                    //
                    // If writing on stdin's pipe would block, mark the write as pending and continue.
                    // This is required blocking on the write() could lead to a deadlock if the child process
                    // is blocking trying to write on stderr / stdout while the relay tries to write stdin.
                    //

                    if (errno == EWOULDBLOCK || errno == EAGAIN)
                    {
                        assert(PendingStdin.empty());
                        PendingStdin.assign(Buffer.begin(), Buffer.begin() + BytesRead);
                    }
                    else
                    {
                        LOG_ERROR("write failed {}", errno);
                        break;
                    }
                }
            }
        }

        //
        // Relay output from the stdout and stderr pipes.
        //

        for (Index = 1; Index < 3; Index += 1)
        {
            if (PollDescriptors[Index].revents & (POLLIN | POLLHUP | POLLERR))
            {
                BytesRead = UtilReadBuffer(PollDescriptors[Index].fd, Buffer);
                if (BytesRead <= 0)
                {
                    if (BytesRead < 0)
                    {
                        LOG_ERROR("read failed {} {}", BytesRead, errno);
                    }

                    PollDescriptors[Index].fd = -1;
                    UtilSocketShutdown(Sockets[Index].get(), SHUT_WR);
                    continue;
                }

                BytesWritten = UtilWriteBuffer(Sockets[Index].get(), Buffer.data(), BytesRead);
                if (BytesWritten < 0)
                {
                    if (errno == EPIPE)
                    {
                        CLOSE(PollDescriptors[Index].fd);
                        PollDescriptors[Index].fd = -1;

                        if (Index == 1)
                        {
                            StdOutPipe.read().reset();
                        }
                        else if (Index == 2)
                        {
                            StdErrPipe.read().reset();
                        }
                    }
                    else
                    {
                        LOG_ERROR("write failed {}, index={}, ChildPid={}, fd={}", errno, Index, ChildPid, Sockets[Index].get());
                    }
                }
            }
        }

        //
        // Relay output from the PTY master to the stdout or stderr socket.
        //

        if (PollDescriptors[3].revents & (POLLIN | POLLHUP | POLLERR))
        {
            BytesRead = UtilReadBuffer(Master, Buffer);

            //
            // N.B. The pty will fail with EIO on read on hangup instead of
            //      indicating EOF.
            //

            if (BytesRead == 0 || (BytesRead < 0 && errno == EIO))
            {
                PollDescriptors[3].fd = -1;
                if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdOutConsole))
                {
                    UtilSocketShutdown(Sockets[1].get(), SHUT_WR);
                }

                if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdErrConsole))
                {
                    UtilSocketShutdown(Sockets[2].get(), SHUT_WR);
                }
            }
            else if (BytesRead < 0)
            {
                LOG_ERROR("read failed {} {}", BytesRead, errno);
                break;
            }
            else
            {
                if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdOutConsole))
                {
                    BytesWritten = UtilWriteBuffer(Sockets[1].get(), Buffer.data(), BytesRead);
                }
                else if (WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsStdErrConsole))
                {
                    BytesWritten = UtilWriteBuffer(Sockets[2].get(), Buffer.data(), BytesRead);
                }
                else
                {
                    LOG_ERROR("Unexpected output from PTY master");
                }

                if (BytesWritten < 0)
                {
                    LOG_ERROR("write failed {}", errno);
                    break;
                }
            }
        }

        //
        // Ensure all data has been written.
        //

        if (BytesWritten > 0)
        {
            continue;
        }

        //
        // Handle interop requests by relaying create process messages from
        // children over the control channel.
        //

        if (PollDescriptors[4].revents & POLLIN)
        {

            wsl::shared::SocketChannel channel(InteropServer.Accept(), "InteropRelay");
            if (channel.Socket() < 0)
            {
                continue;
            }

            auto [Header, Span] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (Header != nullptr)
            {
                try
                {
                    ConfigHandleInteropMessage(
                        channel, ControlChannel, WI_IsFlagSet(CreateProcess.Common.Flags, LxInitCreateProcessFlagsElevated), Span, Header, Config);
                }
                CATCH_LOG();
            }
        }

        //
        // Handle signalfd.
        //

        if (PollDescriptors[5].revents & POLLIN)
        {
            BytesRead = TEMP_FAILURE_RETRY(read(PollDescriptors[5].fd, &SignalInfo, sizeof(SignalInfo)));
            if (BytesRead != sizeof(SignalInfo))
            {
                LOG_ERROR("read failed {} {}", BytesRead, errno);
                break;
            }

            if (SignalInfo.ssi_signo != SIGCHLD)
            {
                LOG_ERROR("Unexpected signal {}", SignalInfo.ssi_signo);
                break;
            }

            //
            // Reap any zombie child processes.
            //

            for (;;)
            {
                Result = waitpid(-1, &Status, WNOHANG);
                if (Result <= 0)
                {
                    break;
                }

                //
                // If the child process exits, write the exit status message
                // via the control channel and shut down the stdin / stdout /
                // stderr sockets.
                //

                if (ChildPid == Result)
                {
                    if (WIFEXITED(Status))
                    {
                        Status = WEXITSTATUS(Status);
                    }

                    try
                    {

                        ExitStatus.Header.MessageType = LxInitMessageExitStatus;
                        ExitStatus.Header.MessageSize = sizeof(ExitStatus);
                        ExitStatus.ExitCode = Status;
                        ControlChannel.SendMessage(ExitStatus);

                        // The result is purposefully ignored here.
                        ControlChannel.ReceiveMessage<LX_INIT_PROCESS_EXIT_STATUS>();
                    }
                    catch (...)
                    {
                        Result = -1;
                        LOG_ERROR("Failed to write exit status {}", errno);
                        break;
                    }

                    ChildPid = -1;
                    UtilSocketShutdown(Sockets[0].get(), SHUT_RD);
                    UtilSocketShutdown(Sockets[1].get(), SHUT_WR);
                    UtilSocketShutdown(Sockets[2].get(), SHUT_WR);
                    PollDescriptors[6].fd = -1;
                }
            }

            //
            // Exit the relay if no more children exist.
            //

            if (Result < 0)
            {
                if (errno != ECHILD)
                {
                    LOG_ERROR("waitpid failed {}", errno);
                }

                break;
            }
        }

        //
        // Process messages from wsl.exe / wslhost.exe.
        //

        if (PollDescriptors[6].revents & POLLIN)
        {
            auto [Message, _] = TerminalControlChannel.ReceiveMessageOrClosed<LX_INIT_WINDOW_SIZE_CHANGED>();

            //
            // A zero-byte read means that the control channel has been closed
            // and that the relay process should exit.
            //

            if (Message == nullptr)
            {
                break;
            }

            memset(&WindowSize, 0, sizeof(WindowSize));
            WindowSize.ws_col = Message->Columns;
            WindowSize.ws_row = Message->Rows;
            Result = ioctl(Master, TIOCSWINSZ, &WindowSize);
            if (Result < 0)
            {
                LOG_ERROR("ioctl(TIOCSWINSZ) failed {}", errno);
            }
        }
    }

    //
    // Cleanly shut down the sockets.
    //
    // N.B. If the socket has already been shut down, this is a no-op.
    //

    UtilSocketShutdown(Sockets[0].get(), SHUT_RD);
    UtilSocketShutdown(Sockets[1].get(), SHUT_WR);
    UtilSocketShutdown(Sockets[2].get(), SHUT_WR);
    UtilSocketShutdown(Sockets[3].get(), SHUT_RD);
    UtilSocketShutdown(Sockets[4].get(), SHUT_WR);
    Result = 0;

CreateProcessUtilityVmEnd:
    if (ListenSocket != -1)
    {
        CLOSE(ListenSocket);
    }

    if (Master != -1)
    {
        CLOSE(Master);
    }

    if (SignalFd != -1)
    {
        CLOSE(SignalFd);
    }

    if (StdIn != -1)
    {
        CLOSE(StdIn);
    }

    if (TtyFd != -1)
    {
        CLOSE(TtyFd);
    }

    //
    // The interop server needs to be manually reset so it deletes
    // its interop socket. See https://github.com/microsoft/WSL/issues/7506.
    //

    InteropServer.Reset();

    //
    // The relay process should always exit.
    //

    if (RelayPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

void InitEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the entry point for the init process.

Arguments:

    Argc - Supplies command line argument count.

    Argv - Supplies command line arguments.

Return Value:

    None.

--*/

{
    //
    // Initialize the startup environment.
    //

    try
    {
        wil::ScopedWarningsCollector collector;

        auto config = ConfigInitializeCommon(g_SavedSignalActions);

        //
        // Check if the binary is being run on WSL or in a Utility VM.
        //

        if (!UtilIsUtilityVm())
        {
            InitEntryWsl(config);
        }
        else
        {
            InitEntryUtilityVm(config);
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    FATAL_ERROR("Init not expected to exit");
    return;
}

void InitEntryUtilityVm(wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is the entry point for the init process when running inside
    a utility VM.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    UtilSetThreadName("init-distro");

    //
    // Set the close-on-exec flag on the socket file descriptor inherited from mini_init.
    //

    wsl::shared::SocketChannel channel{wil::unique_fd{LX_INIT_UTILITY_VM_INIT_SOCKET_FD}, "init"};
    if (fcntl(channel.Socket(), F_SETFD, FD_CLOEXEC) < 0)
    {
        FATAL_ERROR("fcntl failed {}", errno);
        return;
    }

    if (getenv(LX_WSL2_DISTRO_READ_ONLY_ENV) != nullptr)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageReadOnlyDistro());
        unsetenv(LX_WSL2_DISTRO_READ_ONLY_ENV);
    }

    auto Value = getenv(LX_WSL2_NETWORKING_MODE_ENV);
    if (Value != nullptr)
    {
        Config.NetworkingMode = static_cast<LX_MINI_INIT_NETWORKING_MODE>(std::atoi(Value));
        unsetenv(LX_WSL2_NETWORKING_MODE_ENV);
    }

    Value = getenv(LX_WSL2_VM_ID_ENV);
    if (Value != nullptr)
    {
        Config.VmId = Value;

        //
        // Unset the environment variable for user distros.
        //

        Value = getenv(LX_WSL2_SHARED_MEMORY_OB_DIRECTORY);
        if (!Value)
        {
            unsetenv(LX_WSL2_VM_ID_ENV);
        }
    }

    //
    // If the boot.systemd option is specified in /etc/wsl.conf, launch the distro init process as pid 1.
    // WSL init and session leaders continue as children of the distro init process.
    //

    const auto pid = getenv(LX_WSL_PID_ENV);
    assert(pid != nullptr);
    unsetenv(LX_WSL_PID_ENV);

    //
    // Send the create instance result to the service.
    //

    wsl::shared::MessageWriter<LX_MINI_INIT_CREATE_INSTANCE_RESULT> message;
    message->Pid = std::stoul(pid);
    message->Result = 0;

    auto Warnings = wil::ScopedWarningsCollector::ConsumeWarnings();
    if (!Warnings.empty())
    {
        message.WriteString(message->WarningsOffset, Warnings);
    }

    channel.SendMessage<LX_MINI_INIT_CREATE_INSTANCE_RESULT>(message.Span());

    std::optional<pid_t> distroInitPid;
    const auto distroInitPidString = getenv(LX_WSL2_DISTRO_INIT_PID);
    if (distroInitPidString != nullptr)
    {
        distroInitPid = std::stoul(distroInitPidString);
        unsetenv(LX_WSL2_DISTRO_INIT_PID);
    }

    std::vector<gsl::byte> Buffer;
    if (Config.BootInit)
    {
        int SocketPair[2];
        if (socketpair(AF_UNIX, (SOCK_STREAM | SOCK_CLOEXEC), 0, SocketPair) < 0)
        {
            FATAL_ERROR("socketpair failed {}", errno);
        }

        wil::unique_fd BootStartReadSocket{SocketPair[0]};
        Config.BootStartWriteSocket = SocketPair[1];

        const int ChildPid = fork();
        if (ChildPid < 0)
        {
            FATAL_ERROR("fork failed {}", errno);
        }
        else if (ChildPid != 0)
        {
            UtilSetThreadName("init-systemd");

            //
            // Wait to boot the distro init process until the first session leader has been created.
            // This ensures that the entire boot is not done when a distro is trigger-started by accessing \\wsl.localhost.
            //

            auto Message = wsl::shared::socket::RecvMessage(BootStartReadSocket.get(), Buffer);
            if (Message.empty())
            {
                FATAL_ERROR("recv failed {}", errno);
            }

            auto* StartMessage = gslhelpers::get_struct<MESSAGE_HEADER>(Message);
            if (StartMessage->MessageType != LxInitMessageStartDistroInit)
            {
                FATAL_ERROR("unexpected Messagetype {}", StartMessage->MessageType);
            }

            //
            // Initialize distro init arguments and environment.
            //

            auto InitializeStringVector = [&](std::vector<const char*>& PointerVector,
                                              std::vector<std::string>& StringVector,
                                              const std::optional<std::string>& String) {
                if (String.has_value())
                {
                    std::string_view StringView{String.value()};
                    while (!StringView.empty())
                    {
                        StringVector.emplace_back(UtilStringNextToken(StringView, " "));
                    }

                    for (const auto& TokenString : StringVector)
                    {
                        PointerVector.push_back(TokenString.c_str());
                    }
                }

                PointerVector.push_back(nullptr);
            };

            CreateWslSystemdUnits(Config);

            const char* Argv[] = {INIT_PATH, nullptr};
            std::vector<const char*> Env;
            std::vector<std::string> Environment;
            InitializeStringVector(
                Env, Environment, "container=wsl container_host_id=windows container_host_version_id=" WSL_PACKAGE_VERSION);

            execvpe(INIT_PATH, const_cast<char**>(Argv), const_cast<char**>(Env.data()));
            LOG_ERROR("execvpe({}) failed {}", INIT_PATH, errno);
            _exit(1);
        }

        //
        // Keep track of the new pid for WSL init.
        //

        Config.InitPid = getpid();
    }

    //
    // Loop waiting on the socket for requests from the Windows server.
    // A zero-byte read means that the connection to the wsl has been closed and the init daemon should shut down.
    //

    wil::unique_fd SignalFd;
    std::vector<pollfd> PollDescriptors(1);
    PollDescriptors[0].fd = channel.Socket();
    PollDescriptors[0].events = POLLIN;

    //
    // If a distro init pid was passed, set up a signalfd to watch it so the distribution can be terminated
    // when that process exits.
    //

    if (distroInitPid.has_value())
    {
        // Reset sigchld so we get notified when children exit.
        signal(SIGCHLD, SIG_DFL);

        sigset_t SignalMask;
        sigemptyset(&SignalMask);
        sigaddset(&SignalMask, SIGCHLD);
        if (UtilSaveBlockedSignals(SignalMask) < 0)
        {
            FATAL_ERROR("sigprocmask failed {}", errno);
        }

        SignalFd = {signalfd(-1, &SignalMask, SFD_CLOEXEC)};
        if (!SignalFd)
        {
            FATAL_ERROR("signalfd failed {}", errno);
        }

        PollDescriptors.resize(2);
        PollDescriptors[1].fd = SignalFd.get();
        PollDescriptors[1].events = POLLIN;
    }

    for (;;)
    {
        auto Result = poll(PollDescriptors.data(), PollDescriptors.size(), -1);
        if (Result < 0)
        {
            FATAL_ERROR("poll failed {}", errno);
        }

        if (PollDescriptors[0].revents & (POLLHUP | POLLERR))
        {
            break;
        }
        else if (PollDescriptors[0].revents & POLLIN)
        {
            auto [Header, Span] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (Header == nullptr)
            {
                break;
            }

            switch (Header->MessageType)
            {
            case LxInitMessageCreateSession:
                if (InitCreateSessionLeader(Span, channel, -1, Config) < 0)
                {
                    FATAL_ERROR("InitCreateSessionLeader failed");
                }

                break;

            case LxInitMessageInitialize:
                ConfigInitializeInstance(channel, Span, Config);
                break;

            case LxInitMessageTimezoneInformation:
                UpdateTimezone(Span, Config);
                break;

            case LxInitMessageRemountDrvfs:

                //
                // If systemd is enabled, some units (like snapd) might be in the process
                // of creating mountpoints.
                // Because these mountpoints should be available in both namespaces, the elevated
                // and non-elevated namespaces shouldn't fork until systemd is done initializing.
                //

                WaitForBootProcess(Config);
                ConfigRemountDrvFs(Span, channel, Config);
                break;

            case LxInitMessageTerminateInstance:
                InitTerminateInstance(Span, channel, Config);
                break;

            case LxInitCreateProcess:
                ProcessCreateProcessMessage(channel, Span);
                break;

            default:
                FATAL_ERROR("Unexpected message {}", Header->MessageType);
            }
        }

        if (distroInitPid.has_value() && PollDescriptors[1].revents & POLLIN)
        {
            signalfd_siginfo SignalInfo{};
            auto BytesRead = TEMP_FAILURE_RETRY(read(PollDescriptors[1].fd, &SignalInfo, sizeof(SignalInfo)));
            if (BytesRead != sizeof(SignalInfo))
            {
                FATAL_ERROR("read failed {} {}", BytesRead, errno);
            }

            if (SignalInfo.ssi_signo != SIGCHLD)
            {
                LOG_ERROR("Unexpected signal {}", SignalInfo.ssi_signo);
                continue;
            }

            int Status{};
            auto Pid = waitpid(-1, &Status, WNOHANG);
            if (Result == 0)
            {
                continue;
            }
            else if (Result > 0)
            {
                if (Pid == distroInitPid.value())
                {
                    LOG_ERROR("Init has exited. Terminating distribution");
                    break;
                }
            }
            else if (errno != ECHILD)
            {
                FATAL_ERROR("waitpid failed {}", errno);
            }
        }
    }

    InitTerminateInstanceInternal(Config);
    return;
}

void InitEntryWsl(wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is the entry point for the init process when running inside
    WSL.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    auto Warnings = wil::ScopedWarningsCollector::ConsumeWarnings();
    if (!Warnings.empty())
    {
        LOG_ERROR("{}", Warnings.c_str());
    }

    //
    // Connect to the windows server.
    //

    wil::unique_fd LxBusFd = TEMP_FAILURE_RETRY(open(LXBUS_DEVICE_NAME, O_RDWR | O_CLOEXEC));
    if (!LxBusFd)
    {
        FATAL_ERROR("open({}) failed {}", LXBUS_DEVICE_NAME, errno);
        return;
    }

    wsl::shared::SocketChannel Channel{wil::unique_fd{InitConnectToServer(LxBusFd.get(), true)}, "init"};
    if (Channel.Socket() < 0)
    {
        return;
    }

    //
    // Loop waiting on the message port for requests from the Windows server.
    //

    std::vector<gsl::byte> Buffer;
    ssize_t BytesRead;
    for (;;)
    {
        BytesRead = UtilReadMessageLxBus(Channel.Socket(), Buffer, true);
        if (BytesRead < 0)
        {
            return;
        }

        auto Message = gsl::make_span(Buffer.data(), BytesRead);
        auto* Header = gslhelpers::try_get_struct<MESSAGE_HEADER>(Message);
        if (!Header)
        {
            FATAL_ERROR("Invalid message size {}", Message.size());
        }

        switch (Header->MessageType)
        {
        case LxInitMessageCreateSession:
            if (InitCreateSessionLeader(Message, Channel, LxBusFd.get(), Config) < 0)
            {
                //
                // If this distro has no children, exit on failure.
                //

                int status;
                if (waitpid(-1, &status, WNOHANG) == -1 && (errno == ECHILD))
                {
                    FATAL_ERROR("InitCreateSessionLeader failed");
                }

                LOG_ERROR("InitCreateSessionLeader failed");
            }

            break;

        case LxInitMessageNetworkInformation:
            ConfigUpdateNetworkInformation(Message, Config);
            break;

        case LxInitMessageInitialize:
            ConfigInitializeInstance(Channel, Message, Config);
            break;

        case LxInitMessageTimezoneInformation:
            UpdateTimezone(Message, Config);
            break;

        case LxInitMessageTerminateInstance:
            InitTerminateInstance(Message, Channel, Config);
            break;

        default:
            FATAL_ERROR("Unexpected message {}", Header->MessageType);
        }
    }

    return;
}

void InitTerminateInstance(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine processes a terminate instance request from the service.

Arguments:

    Buffer - Supplies the message buffer.

    Channel - Supplies a channel to send the response.

    Config - Supplies the distribution config.

Return Value:

    None.

--*/
try
{

    auto* Message = gslhelpers::try_get_struct<LX_INIT_TERMINATE_INSTANCE>(Buffer);
    if (!Message)
    {
        FATAL_ERROR("Invalid message size {}", Buffer.size());
    }

    //
    // Attempt to stop the plan9 server, if it is not able to be stopped because of an
    // in-use file, reply to the service that the instance could not be terminated.
    //

    if (!StopPlan9Server(Message->Force, Config))
    {
        Channel.SendResultMessage<bool>(false);
        return;
    }

    InitTerminateInstanceInternal(Config);
}
CATCH_LOG();

void InitTerminateInstanceInternal(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine attempts to cleanly terminate the instance.

Arguments:

    Config - Supplies the distribution config.

Return Value:

    None.

--*/
try
{
    //
    // If systemd is enabled, attempt to poweroff the instance via systemctl.
    //

    if (Config.BootInit && !Config.BootStartWriteSocket)
    {
        THROW_LAST_ERROR_IF(UtilSetSignalHandlers(g_SavedSignalActions, false) < 0);

        if (UtilExecCommandLine("systemctl poweroff", nullptr) == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(Config.BootInitTimeout));
            LOG_ERROR("systemctl poweroff did not terminate the instance in {} ms, calling reboot(RB_POWER_OFF)", Config.BootInitTimeout);
        }
    }

    reboot(RB_POWER_OFF);
    FATAL_ERROR("reboot(RB_POWER_OFF) failed {}", errno);
}
CATCH_LOG();

void InstallSystemdUnit(const char* Path, const std::string& Name, const char* Content)
try
{
    std::string target = std::format("{}/{}.service", Path, Name);
    std::string defaultTarget = std::format("{}/default.target.wants", Path);
    THROW_LAST_ERROR_IF(UtilMkdirPath(Path, 0755) < 0);
    THROW_LAST_ERROR_IF(WriteToFile(target.c_str(), Content) < 0);
    THROW_LAST_ERROR_IF(UtilMkdirPath(defaultTarget.c_str(), 0755) < 0);

    std::string symlinkPath = std::format("{}/{}.service", defaultTarget, Name);
    THROW_LAST_ERROR_IF(symlink(target.c_str(), symlinkPath.c_str()) < 0);
}
CATCH_LOG();

void CreateWslSystemdUnits(const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This method creates systemd unit files to protect WSL functionality from being disabled by systemd.

Arguments:

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

try
{
    if (Config.NetworkingMode == LxMiniInitNetworkingModeMirrored)
    {
        HardenMirroredNetworkingSettingsAgainstSystemd();
    }

    constexpr auto folder = "/run/systemd/system-generators";

    THROW_LAST_ERROR_IF(UtilMkdirPath(folder, 0755) < 0);
    THROW_LAST_ERROR_IF(symlink("/init", std::format("{}/{}", folder, LX_INIT_WSL_GENERATOR).c_str()));

    if (Config.GuiAppsEnabled)
    {
        constexpr auto folder = "/run/systemd/user-generators";

        THROW_LAST_ERROR_IF(UtilMkdirPath(folder, 0755) < 0);
        THROW_LAST_ERROR_IF(symlink("/init", std::format("{}/{}", folder, LX_INIT_WSL_USER_GENERATOR).c_str()));
    }
}
CATCH_LOG();

void HardenMirroredNetworkingSettingsAgainstSystemd()

/*++

Routine Description:

    This routine writes configuration required for the mirrored networking mode loopback datapath to
    a .conf file applied by systemd. Some distros come with default .conf files only applied when
    systemd is enabled, and some of these contain network configurations that conflict with mirrored
    networking mode. By writing to a .conf file that has higher precedence, we can prevent these
    conflicting settings from being applied.

Arguments:

    None.

Return Value:

    None.

--*/

try
{
    const char* NetworkingConfigFileDirectory = "/run/sysctl.d";
    const char* NetworkingConfigFileName = "wsl-networking.conf";
    const std::string NetworkingConfigFilePath = std::format("{}/{}", NetworkingConfigFileDirectory, NetworkingConfigFileName);
    constexpr auto NetworkingConfig =
        "# Note: This file is generated by WSL to prevent default .conf files applied by systemd from overwriting critical "
        "networking settings\n"
        "net.ipv4.conf.all.rp_filter=0\n"
        "net.ipv4.conf." LX_INIT_LOOPBACK_DEVICE_NAME ".rp_filter=0\n";

    THROW_LAST_ERROR_IF(UtilMkdirPath(NetworkingConfigFileDirectory, 0755) < 0);
    THROW_LAST_ERROR_IF(WriteToFile(NetworkingConfigFilePath.c_str(), NetworkingConfig) < 0);
}
CATCH_LOG();

void SessionLeaderCreateProcess(gsl::span<gsl::byte> Buffer, int MessageFd, int TtyFd, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine creates a process from a session leader.

Arguments:

    Buffer - Supplies the message buffer.

    MessageFd - Supplies a message port file descriptor.

    TtyFd - Supplies a Tty file descriptor.

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    //
    // Parse the create process message buffer and create the new child process.
    //

    CREATE_PROCESS_PARSED Parsed = CreateProcessParse(Buffer, MessageFd, Config);
    auto CreateProcessPid = fork();
    THROW_LAST_ERROR_IF(CreateProcessPid < 0);

    if (CreateProcessPid > 0)
    {
        //
        // Remember the current process group.
        //

        if (g_SessionGroup == -1)
        {
            g_SessionGroup = CreateProcessPid;
        }

        //
        // Reply with pid of the child process.
        //

        THROW_LAST_ERROR_IF(CreateProcessReplyToServer(&Parsed, CreateProcessPid, MessageFd) < 0);

        return;
    }

    //
    // Child...
    //
    // The child process should be part of a separate foreground process group.
    // If a separate foreground process group does not exist, create one here.
    //

    int Result = 0;
    if (g_SessionGroup != -1)
    {
        //
        // Attempt to join an existing foreground process group.
        //

        Result = setpgid(0, g_SessionGroup);
    }

    if ((g_SessionGroup == -1) || (Result < 0))
    {
        //
        // Create a new process group.
        //

        THROW_LAST_ERROR_IF(setpgid(0, 0) < 0);
    }

    //
    // Always bring the process group to the foreground. This will give
    // the newly launched process access to the terminal. In cases where
    // multiple processes are being launched in the same session due to
    // piping commands together (e.g. bash.exe -c ls | bash.exe -c less)
    // or calling bash.exe from within a running WSL instance (inception),
    // there may be issues when this process terminates, as restoring the
    // foreground group does not happen by default. If the launcher is a
    // shell program like /bin/bash, then it typically assumes that the
    // foreground needs to be restored and all will work well.
    //

    //
    // N.B. SIGTTOU along with most other signals are blocked. Otherwise,
    //      this could generate a signal with the default behavior of
    //      stopping the process (waiting for SIGCONT to continue).
    //

    if (tcsetpgrp(TtyFd, getpgid(0)) < 0)
    {
        LOG_ERROR("tcsetpgrp failed {}", errno);
    }

    //
    // Exec the new process.
    //

    //
    // Resources are not released for the child process because it will call execv.
    //
    // N.B. CreateProcess does not return.
    //

    CreateProcess(&Parsed, TtyFd, Config);
    FATAL_ERROR("CreateProcess not expected to return");
}

void SessionLeaderSigchldHandler(__attribute__((unused)) int Signal, __attribute__((unused)) siginfo_t* SigInfo, __attribute__((unused)) void* UContext)

/*++

Routine Description:

    This routine determines if the process group assigned to processes launched
    by the session leader has terminated.

Arguments:

    Signal - Supplies the signal that was received.

    SigInfo - Supplies additional information about the signal.

    UContext - Supplies the scheduling context from the process before the
        signal handler was invoked.

Return Value:

    None.

--*/

{
    pid_t child;
    int status;

    while ((child = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (child == g_SessionGroup)
        {
            g_SessionGroup = -1;
        }
    }

    return;
}

void SessionLeaderEntryUtilityVm(wsl::shared::SocketChannel& channel, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is the entry point for the session leader process.

Arguments:

    channel - Supplies a message channel

Return Value:

    None.

--*/

{
    std::vector<gsl::byte> Buffer;
    MESSAGE_HEADER* Header{};
    struct sigaction SignalAction;

    //
    // Create a new session.
    //

    if (setsid() < 0)
    {
        FATAL_ERROR("setsid failed {}", errno);
    }

    //
    // Set up a signal handler to reap child processes and track session
    // leader.
    //

    memset(&SignalAction, 0, sizeof(SignalAction));
    SignalAction.sa_flags = SA_SIGINFO;
    SignalAction.sa_sigaction = SessionLeaderSigchldHandler;
    if (sigaction(SIGCHLD, &SignalAction, NULL) < 0)
    {
        FATAL_ERROR("sigaction SIGCHLD failed {}", errno);
    }

    //
    // Loop waiting on the socket for requests from the Windows server. A zero-byte read means that there is no
    // longer any active console applications for this session and that the session leader should exit.
    //

    for (;;)
    {
        auto [Message, Span] = channel.ReceiveMessageOrClosed<LX_INIT_CREATE_PROCESS_UTILITY_VM>();
        if (Message == nullptr)
        {
            _exit(0);
        }

        switch (Message->Header.MessageType)
        {
        case LxInitMessageCreateProcessUtilityVm:
            if (InitCreateProcessUtilityVm(Span, *Message, channel, Config) < 0)
            {
                FATAL_ERROR("InitCreateProcessUtilityVm failed");
            }

            break;

        default:
            FATAL_ERROR("Unexpected message {}", Header->MessageType);
        }
    }

    FATAL_ERROR("Session leader not expected to exit");
    return;
}

void SessionLeaderEntry(int MessageFd, int TtyFd, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine is the entry point for the session leader process.

Arguments:

    MessageFd - Supplies a message port file descriptor.

    TtyFd - Supplies a Tty file descriptor.

    Config - Supplies the distribution configuration.

Return Value:

    None.

--*/

{
    std::vector<gsl::byte> Buffer;
    ssize_t BytesRead;
    struct sigaction SignalAction;

    //
    // Create a new session and set the controlling session on the Tty device.
    //

    if (setsid() < 0)
    {
        FATAL_ERROR("setsid failed {}", errno);
    }

    if (TEMP_FAILURE_RETRY(ioctl(TtyFd, TIOCSCTTY, NULL)) < 0)
    {
        FATAL_ERROR("ioctl failed for TIOCSCTTY {}", errno);
    }

    //
    // Set up a signal handler to reap child processes and track session
    // leader.
    //

    memset(&SignalAction, 0, sizeof(SignalAction));
    SignalAction.sa_flags = SA_SIGINFO;
    SignalAction.sa_sigaction = SessionLeaderSigchldHandler;
    if (sigaction(SIGCHLD, &SignalAction, NULL) < 0)
    {
        FATAL_ERROR("sigaction SIGCHLD failed {}", errno);
    }

    //
    // Loop waiting on the message port for requests from the Windows server.
    //

    for (;;)
    {
        BytesRead = UtilReadMessageLxBus(MessageFd, Buffer, false);
        if (BytesRead < 0)
        {
            FATAL_ERROR("read failed {}", errno);
        }

        auto Message = gsl::make_span(Buffer.data(), BytesRead);
        auto* Header = gslhelpers::try_get_struct<MESSAGE_HEADER>(Message);
        if (!Header)
        {
            FATAL_ERROR("Invalid message size {}", Message.size());
        }

        if (Header->MessageType == LxInitMessageCreateProcess)
        {
            SessionLeaderCreateProcess(Message, MessageFd, TtyFd, Config);
        }
        else
        {
            FATAL_ERROR("Unexpected message {}", Header->MessageType);
        }
    }

    FATAL_ERROR("Session leader not expected to exit");
    return;
}

bool StopPlan9Server(bool Force, wsl::linux::WslDistributionConfig& Config)
{
    if (Config.Plan9ControlChannel.Socket() < 0)
    {
        return true;
    }

    LX_INIT_STOP_PLAN9_SERVER Message{};
    Message.Header.MessageType = LxInitMessageStopPlan9Server;
    Message.Header.MessageSize = sizeof(Message);
    Message.Force = Message.Force;

    const auto& Response = Config.Plan9ControlChannel.Transaction(Message);

    if (Response.Result)
    {
        // The plan9 server is terminated, release the socket.
        Config.Plan9ControlChannel.Close();
    }

    return Response.Result;
}

wil::unique_fd UnmarshalConsoleFromServer(int MessageFd, LXBUS_IPC_CONSOLE_ID ConsoleId)

/*++

Routine Description:

    This routine unmarshals a console.

Arguments:

    MessageFd - Supplies a message port file descriptor.

    ConsoleId - Supplies a console ID.

    TtyFd - Supplies a buffer to store a Tty file descriptor.

Return Value:

    0 on success, -1 on failure.

--*/

{
    LXBUS_IPC_MESSAGE_UNMARSHAL_CONSOLE_PARAMETERS UnmarshalConsole{};

    //
    // N.B. Failures to unmarshall the console are non-fatal.
    //

    UnmarshalConsole.Input.ConsoleId = ConsoleId;

    if (TEMP_FAILURE_RETRY(ioctl(MessageFd, LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_CONSOLE, &UnmarshalConsole)))
    {
        LOG_ERROR("Failed to unmarshal console {}", errno);
        return {};
    }

    return UnmarshalConsole.Output.FileDescriptor;
}

unsigned int StartPlan9(int Argc, char** Argv)
{
    constexpr auto* Usage = "Usage: plan9 " LX_INIT_PLAN9_CONTROL_SOCKET_ARG " fd " LX_INIT_PLAN9_SOCKET_PATH_ARG
                            " path " LX_INIT_PLAN9_SERVER_FD_ARG " fd " LX_INIT_PLAN9_LOG_FILE_ARG
                            " log-file " LX_INIT_PLAN9_LOG_LEVEL_ARG " level " LX_INIT_PLAN9_PIPE_FD_ARG " fd [--log-truncate]\n";

    bool LogTruncate = false;
    int LogLevel = TRACE_LEVEL_INFORMATION;
    wil::unique_fd PipeFd;
    const char* SocketPath{};
    const char* LogFile{};
    wil::unique_fd ControlSocket;
    wil::unique_fd ServerFd;

    ArgumentParser parser(Argc, Argv);
    parser.AddArgument(UniqueFd{ControlSocket}, LX_INIT_PLAN9_CONTROL_SOCKET_ARG);
    parser.AddArgument(SocketPath, LX_INIT_PLAN9_SOCKET_PATH_ARG);
    parser.AddArgument(UniqueFd{ServerFd}, LX_INIT_PLAN9_SERVER_FD_ARG);
    parser.AddArgument(LogFile, LX_INIT_PLAN9_LOG_FILE_ARG);
    parser.AddArgument(Integer{LogLevel}, LX_INIT_PLAN9_LOG_LEVEL_ARG);
    parser.AddArgument(UniqueFd{PipeFd}, LX_INIT_PLAN9_PIPE_FD_ARG);
    parser.AddArgument(LogTruncate, LX_INIT_PLAN9_TRUNCATE_LOG_ARG);

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        std::cerr << e.what() << "\n" << Usage;
        return 1;
    }

    RunPlan9Server(SocketPath, LogFile, LogLevel, LogTruncate, ControlSocket.get(), ServerFd.get(), PipeFd);

    return 0;
}

unsigned int StartGns(int Argc, char** Argv)
{
    constexpr auto* Usage =
        "Usage: gns [" LX_INIT_GNS_SOCKET_ARG " fd] [" LX_INIT_GNS_DNS_SOCKET_ARG " fd] [" LX_INIT_GNS_ADAPTER_ARG
        " guid] [" LX_INIT_GNS_MESSAGE_TYPE_ARG " int] [" LX_INIT_GNS_DNS_TUNNELING_IP " ip]\n";

    UtilSetThreadName("GNS");

    // Initialize error and telemetry logging.
    InitializeLogging(false);

    // hvsocket file descriptor used for DNS tunneling
    std::optional<int> DnsFd;
    std::optional<GUID> AdapterId;
    std::optional<LX_MESSAGE_TYPE> MessageType;
    std::string DnsTunnelingIp;
    wil::unique_fd Socket;

    ArgumentParser parser(Argc, Argv);
    parser.AddArgument(UniqueFd{Socket}, LX_INIT_GNS_SOCKET_ARG);
    parser.AddArgument(Integer{DnsFd}, LX_INIT_GNS_DNS_SOCKET_ARG);
    parser.AddArgument(AdapterId, LX_INIT_GNS_ADAPTER_ARG);
    parser.AddArgument(Integer{MessageType}, LX_INIT_GNS_MESSAGE_TYPE_ARG);
    parser.AddArgument(DnsTunnelingIp, LX_INIT_GNS_DNS_TUNNELING_IP);

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        std::cerr << e.what() << "\n" << Usage;
        return 1;
    }

    wsl::shared::SocketChannel channel{std::move(Socket), "GNS"};

    GnsEngine::NotificationRoutine readNotification;
    GnsEngine::StatusRoutine returnStatus;

    // returns the most recent error when init is created for unit tests (i.e. Fd == -1)
    int exitCode = 0;

    if (channel.Socket() == -1)
    {
        readNotification = [&]() -> std::optional<GnsEngine::Message> {
            std::string content{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
            if (content.empty())
            {
                return {};
            }
            if (MessageType.has_value())
            {
                return {{MessageType.value(), content, AdapterId}};
            }

            return {{AdapterId.has_value() ? LxGnsMessageNotification : LxGnsMessageInterfaceConfiguration, content, AdapterId}};
        };

        returnStatus = [&](int Result, const std::string& Error) {
            GNS_LOG_INFO("Returning LxGnsMessageResult (no output fd) [{} - {}]", Result, Error.c_str());
            // exitCode keeps the most recent error in the test path
            if (Result != 0)
            {
                exitCode = Result;
            }
            return true;
        };
    }
    else
    {
        readNotification = [&]() -> std::optional<GnsEngine::Message> {
            std::vector<gsl::byte> Buffer;
            auto [Message, Span] = channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
            if (Message == nullptr)
            {
                return {};
            }

            auto type = Message->MessageType;
            GNS_LOG_INFO("Processing LX_MESSAGE_TYPE {}", ToString(type));
            switch (type)
            {
            case LxGnsMessageNoOp:
            case LxGnsMessageGlobalNetFilter:
            {
                return {{type, {}, {}}};
            }
            case LxGnsMessageInterfaceConfiguration:
            {
                auto size = Span.size() - offsetof(LX_GNS_INTERFACE_CONFIGURATION, Content) - 1;
                assert(size > 0);

                std::string Content{reinterpret_cast<PLX_GNS_INTERFACE_CONFIGURATION>(Span.data())->Content, size};

                return {{type, Content, {}}};
            }

            case LxGnsMessageNotification:
            {
                auto size = Span.size() - offsetof(LX_GNS_NOTIFICATION, Content) - 1;
                assert(size > 0);

                const auto* NotificationMessage = reinterpret_cast<PLX_GNS_NOTIFICATION>(Span.data());
                std::string Content{NotificationMessage->Content, size};
                return {{type, Content, {NotificationMessage->AdapterId}}};
            }

            case LxGnsMessageVmNicCreatedNotification:
            case LxGnsMessageCreateDeviceRequest:
            case LxGnsMessageModifyGuestDeviceSettingRequest:
            case LxGnsMessageLoopbackRoutesRequest:
            case LxGnsMessageInitialIpConfigurationNotification:
            case LxGnsMessageInterfaceNetFilter:
            case LxGnsMessageDeviceSettingRequest:
            case LxGnsMessageSetupIpv6:
            case LxGnsMessageConnectTestRequest:
            {
                auto size = Span.size() - offsetof(LX_GNS_JSON_MESSAGE, Content) - 1;
                if (size == 0)
                {
                    throw RuntimeErrorWithSourceLocation(
                        std::format("Failed to find content for LX_MESSAGE_TYPE : {}", static_cast<int>(type)));
                }

                std::string Content{reinterpret_cast<PLX_GNS_JSON_MESSAGE>(Span.data())->Content, size};
                return {{type, Content, {}}};
            }

            default:
            {
                throw RuntimeErrorWithSourceLocation(std::format("Unexpected LX_MESSAGE_TYPE : {}", static_cast<int>(type)));
            }
            }
        };

        returnStatus = [&](int Result, const std::string& Error) {
            std::vector<gsl::byte> Buffer(sizeof(LX_GNS_RESULT) + Error.size() + 1);

            GNS_LOG_INFO("Returning LxGnsMessageResult [{} - {}]", Result, Error.c_str());

            wsl::shared::MessageWriter<LX_GNS_RESULT> response(LX_GNS_RESULT::Type);
            response->Result = Result;
            if (!Error.empty())
            {
                response.WriteString(Error);
            }

            return channel.SendMessage<LX_GNS_RESULT>(response.Span());
        };
    }

    RoutingTable routingTable(RT_TABLE_MAIN);
    NetworkManager manager(routingTable);
    GnsEngine engine(readNotification, returnStatus, manager, DnsFd, DnsTunnelingIp);

    engine.run();

    GNS_LOG_INFO("StartGns returning {} (GNS Socket {}, MessageType {})", exitCode, channel.Socket(), MessageType.value_or(LxMiniInitMessageAny));
    return exitCode;
}

void WaitForBootProcess(wsl::linux::WslDistributionConfig& Config)
{
    if (!Config.BootStartWriteSocket)
    {
        return;
    }

    //
    // Launch the boot process wait for it to finish booting.
    //

    MESSAGE_HEADER Message{};
    Message.MessageType = LxInitMessageStartDistroInit;
    Message.MessageSize = sizeof(Message);
    if (UtilWriteBuffer(Config.BootStartWriteSocket.get(), gslhelpers::struct_as_bytes(Message)) < 0)
    {
        LOG_ERROR("write failed {}", errno);
    }

    Config.BootStartWriteSocket.reset();
    if (Config.BootInitTimeout > 0)
    {
        try
        {
            //
            // N.B. Init needs to not ignore SIGCHLD so it can wait for the child process.
            //

            signal(SIGCHLD, SIG_DFL);
            auto restoreDisposition = wil::scope_exit([]() { signal(SIGCHLD, SIG_IGN); });
            wsl::shared::retry::RetryWithTimeout<void>(
                [&]() {
                    std::string Output;
                    THROW_LAST_ERROR_IF(
                        UtilExecCommandLine("systemctl is-system-running | grep -E \"running|degraded\"", &Output, 0, false) < 0);
                },
                std::chrono::milliseconds{250},
                std::chrono::milliseconds{Config.BootInitTimeout});
        }
        catch (...)
        {
            LOG_ERROR("{} failed to start within {}ms", INIT_PATH, Config.BootInitTimeout);
        }
    }
}