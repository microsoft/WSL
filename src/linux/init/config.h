/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    config.c

Abstract:

    This file contains information related to configuring a running instance.

--*/

#pragma once

#include <unistd.h>
#include <pwd.h>
#include <optional>
#include <set>
#include <string_view>
#include <optional>
#include "SocketChannel.h"
#include "WslDistributionConfig.h"

#define WSL_USE_VIRTIO_9P(_Config) (WI_IsFlagSet(UtilGetFeatureFlags((_Config)), LxInitFeatureVirtIo9p))
#define WSL_USE_VIRTIO_FS(_Config) (WI_IsFlagSet(UtilGetFeatureFlags((_Config)), LxInitFeatureVirtIoFs))
#define WSLG_SHARED_FOLDER "wslg"

#define INIT_MAKE_SECURITY(_uid, _gid, _mode) {_uid, _gid, _mode}
#define INIT_ANY_SYMLINK(_source, _target) \
    { \
        InitStartupTypeSymlink, \
        { \
            .Symlink = INIT_MAKE_SYMLINK(_source, _target) \
        } \
    }
#define INIT_ANY_DIRECTORY(_path, _uid, _gid, _mode) \
    { \
        InitStartupTypeDirectory, \
        { \
            .Directory = INIT_MAKE_DIRECTORY(_path, _uid, _gid, _mode) \
        } \
    }
#define INIT_ANY_MOUNT(_mount, _filesystem, _flags) \
    { \
        InitStartupTypeMount, \
        { \
            .Mount = INIT_MAKE_MOUNT(_mount, _filesystem, NULL, NULL, INIT_MAKE_SECURITY(0, 0, 0), _flags, false) \
        } \
    }

#define INIT_ANY_MOUNT_OPTION(_mount, _filesystem, _option, _flags) \
    { \
        InitStartupTypeMount, \
        { \
            .Mount = INIT_MAKE_MOUNT(_mount, _filesystem, NULL, (_option), INIT_MAKE_SECURITY(0, 0, 0), _flags, false) \
        } \
    }

#define INIT_ANY_MOUNT_DEVICE(_mount, _filesystem, _device, _flags) \
    { \
        InitStartupTypeMount, \
        { \
            .Mount = INIT_MAKE_MOUNT(_mount, _filesystem, (_device), NULL, INIT_MAKE_SECURITY(0, 0, 0), _flags, false) \
        } \
    }

#define INIT_ANY_MOUNT_DEVICE_OPTION(_mount, _filesystem, _device, _option, _flags) \
    { \
        InitStartupTypeMount, \
        { \
            .Mount = INIT_MAKE_MOUNT(_mount, _filesystem, (_device), (_option), INIT_MAKE_SECURITY(0, 0, 0), _flags, false) \
        } \
    }

#define INIT_ANY_MOUNT_DEVICE_OPTION_IGNORE_FAILURE(_mount, _filesystem, _device, _option, _flags) \
    { \
        InitStartupTypeMount, \
        { \
            .Mount = INIT_MAKE_MOUNT(_mount, _filesystem, (_device), (_option), INIT_MAKE_SECURITY(0, 0, 0), _flags, true) \
        } \
    }

#define INIT_ANY_NODE(_path, _uid, _gid, _mode, _major, _minor) \
    { \
        InitStartupTypeNode, \
        { \
            .Node = INIT_MAKE_NODE(_path, _uid, _gid, _mode, _major, _minor) \
        } \
    }
#define INIT_ANY_FILE(_filename, _mode) \
    { \
        InitStartupTypeFile, \
        { \
            .File = INIT_MAKE_FILE(_filename, _mode) \
        } \
    }
#define INIT_MAKE_MOUNT(_mount, _filesystem, _devicename, _options, _directorysecurity, _flags, _ignorefailure) \
    {(_mount), (_filesystem), _devicename, _options, _directorysecurity, _flags, _ignorefailure}
#define INIT_MAKE_SYMLINK(_source, _target) {(_source), (_target)}
#define INIT_MAKE_DIRECTORY(_path, _uid, _gid, _mode) \
    { \
        (_path), \
        { \
            _uid, _gid, _mode \
        } \
    }
#define INIT_MAKE_FILE(_filename, _mode) {(_filename), _mode}
#define INIT_MAKE_NODE(_path, _uid, _gid, _mode, _major, _minor) {(_path), {_uid, _gid, _mode}, _major, _minor}

//
// Major devices.
//

#define INIT_DEV_MEM_MAJOR_NUMBER (1)
#define INIT_DEV_TTY_MAJOR_NUMBER (4)
#define INIT_DEV_ALT_TTY_MAJOR_NUMBER (5)
#define INIT_DEV_MISC_MAJOR_NUMBER (10)

//
// LxBus Device.
//
// TODO_LX: The minor number is in the dynamic allocation range for misc
//          devices. Once dynamic allocation is supported it should probably
//          be actually dynamically allocated.
//

#define INIT_DEV_LXBUS_MINOR_NUMBER (50)
#define INIT_DEV_LXBUS_MAJOR_NUMBER INIT_DEV_MISC_MAJOR_NUMBER

//
// Full Device.
//

#define INIT_DEV_FULL_MINOR_NUMBER (7)
#define INIT_DEV_FULL_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// Log devices.
//

#define INIT_DEV_LOG_KMSG_MINOR_NUMBER (11)
#define INIT_DEV_LOG_KMSG_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// Null Device.
//

#define INIT_DEV_NULL_MINOR_NUMBER (3)
#define INIT_DEV_NULL_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// Zero Device.
//

#define INIT_DEV_ZERO_MINOR_NUMBER (5)
#define INIT_DEV_ZERO_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// PTMX Device.
//

#define INIT_DEV_PTM_MINOR_NUMBER (2)
#define INIT_DEV_PTM_MAJOR_NUMBER INIT_DEV_ALT_TTY_MAJOR_NUMBER

//
// PTS Devices.
//

#define INIT_DEV_PTS_MAJOR_NUMBER (136)

//
// Random device.
//

#define INIT_DEV_RANDOM_MINOR_NUMBER (8)
#define INIT_DEV_RANDOM_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// TTY device.
//

#define INIT_DEV_TTY0_MINOR_NUMBER (0)
#define INIT_DEV_TTY_MINOR_NUMBER_FIRST_VIRTUAL (1)
#define INIT_DEV_TTY_MINOR_NUMBER_MAX_VIRTUAL (64)
#define INIT_DEV_TTY_MINOR_NUMBER_FIRST_SERIAL (64)
#define INIT_DEV_TTY_MINOR_NUMBER_MAX_SERIAL (256)

#define INIT_DEV_TTY_SERIAL_MODE (S_IFCHR | 0660)
#define INIT_DEV_TTY_SERIAL_GID DIALOUT_GID
#define INIT_DEV_TTY_SERIAL_UID ROOT_UID
#define INIT_DEV_TTY_SERIAL_FORMAT "/dev/ttyS{}"

#define INIT_DEV_TTYCT_MINOR_NUMBER (0)
#define INIT_DEV_TTYCT_MAJOR_NUMBER INIT_DEV_ALT_TTY_MAJOR_NUMBER

//
// URandom Device.
//

#define INIT_DEV_URANDOM_MINOR_NUMBER (9)
#define INIT_DEV_URANDOM_MAJOR_NUMBER INIT_DEV_MEM_MAJOR_NUMBER

//
// UID and GID values.
//

#define DIALOUT_GID (20)
#define ROOT_GID (0)
#define ROOT_UID (0)
#define TTY_GID (5)
#define TTY_MODE (0660)

//
// Initialization helpers.
//

typedef struct _INIT_SECURITY
{
    uid_t Uid;
    gid_t Gid;
    mode_t Mode;
} INIT_SECURITY, *PINIT_SECURITY;

using PCINIT_SECURITY = const INIT_SECURITY*;

typedef struct _INIT_STARTUP_MOUNT
{
    const char* MountLocation;
    const char* FileSystemType;
    const char* DeviceName;
    const char* MountOptions;
    INIT_SECURITY DirectorySecurity;
    unsigned long Flags;
    bool IgnoreFailure;
} INIT_STARTUP_MOUNT, *PINIT_STARTUP_MOUNT;

using PCINIT_STARTUP_MOUNT = const INIT_STARTUP_MOUNT*;

typedef struct _INIT_STARTUP_SYMBOLIC_LINK
{
    const char* Source;
    const char* Target;
} INIT_STARTUP_SYMBOLIC_LINK, *PINIT_STARTUP_SYMBOLIC_LINK;

using PCINIT_STARTUP_SYMBOLIC_LINK = const INIT_STARTUP_SYMBOLIC_LINK*;

typedef struct _INIT_STARTUP_DIRECTORY
{
    const char* Path;
    INIT_SECURITY Security;
} INIT_STARTUP_DIRECTORY, *PINIT_STARTUP_DIRECTORY;

using PCINIT_STARTUP_DIRECTORY = const INIT_STARTUP_DIRECTORY*;

typedef struct _INIT_STARTUP_FILE
{
    const char* FileName;
    mode_t Mode;
} INIT_STARTUP_FILE, *PINIT_STARTUP_FILE;

using PCINIT_STARTUP_FILE = const INIT_STARTUP_FILE*;

typedef struct _INIT_STARTUP_NODE
{
    const char* Path;
    INIT_SECURITY Security;
    unsigned int MajorNumber;
    unsigned int MinorNumber;
} INIT_STARTUP_NODE, *PINIT_STARTUP_NODE;

using PCINIT_STARTUP_NODE = const INIT_STARTUP_NODE*;

typedef enum _INIT_STARTUP_TYPE
{
    InitStartupTypeDirectory,
    InitStartupTypeMount,
    InitStartupTypeNode,
    InitStartupTypeSymlink,
    InitStartupTypeFile
} INIT_STARTUP_TYPE,
    *PINIT_STARTUP_TYPE;

typedef struct _INIT_STARTUP_ANY
{
    INIT_STARTUP_TYPE Type;

    union
    {
        INIT_STARTUP_DIRECTORY Directory;
        INIT_STARTUP_MOUNT Mount;
        INIT_STARTUP_NODE Node;
        INIT_STARTUP_SYMBOLIC_LINK Symlink;
        INIT_STARTUP_FILE File;
    } u;
} INIT_STARTUP_ANY, *PINIT_STARTUP_ANY;

using PCINIT_STARTUP_ANY = const INIT_STARTUP_ANY*;

class EnvironmentBlock
{
public:
    EnvironmentBlock() = default;

    EnvironmentBlock(const char* Buffer, unsigned short Count)
    {
        m_variables.reserve(Count);
        for (unsigned short Index = 0; Index < Count; Index += 1)
        {
            std::string entry{Buffer};
            const auto size = entry.size();
            m_variables.push_back(std::move(entry));
            Buffer += size + 1;
        }
    }

    void AddVariable(std::string_view Name, std::string_view Value)
    {
        std::string entry{Name};
        entry += "=";
        auto found = std::find_if(
            m_variables.begin(), m_variables.end(), [&entry](const auto& variable) { return (variable.find(entry) == 0); });

        entry += Value;
        if (found != m_variables.end())
        {
            *found = std::move(entry);
        }
        else
        {
            m_variables.push_back(std::move(entry));
        }
    }

    int AddVariableNoThrow(std::string_view Name, std::string_view Value)
    try
    {
        AddVariable(Name, Value);
        return 0;
    }

    CATCH_RETURN_ERRNO()

    std::string_view GetVariable(std::string_view Name)
    {
        std::string prefix{Name};
        prefix += "=";
        auto found = std::find_if(
            m_variables.begin(), m_variables.end(), [&prefix](const auto& variable) { return (variable.find(prefix) == 0); });

        std::string_view value{};
        if (found != m_variables.end())
        {
            value = *found;
            value = value.substr(prefix.size());
        }

        return value;
    }

    std::vector<const char*> Variables()
    {
        std::vector<const char*> variables;
        variables.reserve(m_variables.size() + 1);
        std::for_each(
            m_variables.begin(), m_variables.end(), [&variables](const auto& variable) { variables.push_back(variable.c_str()); });

        variables.push_back(nullptr);
        return variables;
    }

private:
    std::vector<std::string> m_variables;
};

void ConfigAppendToPath(EnvironmentBlock& Environment, std::string_view PathElement);

wsl::linux::WslDistributionConfig ConfigInitializeCommon(struct sigaction* SavedSignalActions);

int ConfigInitializeEntry(PCINIT_STARTUP_ANY AnyEntry);

int ConfigInitializeVmMode(bool Elevated, wsl::linux::WslDistributionConfig& Config);

int ConfigInitializeWsl(void);

void ConfigInitializeX11(const wsl::linux::WslDistributionConfig& Config);

void ConfigCreateResolvConfSymlink(const wsl::linux::WslDistributionConfig& Config);

int ConfigCreateResolvConfSymlinkTarget(void);

EnvironmentBlock ConfigCreateEnvironmentBlock(PLX_INIT_CREATE_PROCESS_COMMON Common, const wsl::linux::WslDistributionConfig& Config);

std::optional<unsigned int> ConfigGetDriveLetter(std::string_view MountSource);

std::set<std::pair<unsigned int, std::string>> ConfigGetMountedDrvFsVolumes(void);

std::vector<std::pair<std::string, std::string>> ConfigGetWslgEnvironmentVariables(const wsl::linux::WslDistributionConfig& Config);

void ConfigHandleInteropMessage(
    wsl::shared::SocketChannel& ResponseChannel,
    wsl::shared::SocketChannel& InteropChannel,
    bool Elevated,
    gsl::span<gsl::byte> Message,
    const MESSAGE_HEADER* Header,
    const wsl::linux::WslDistributionConfig& Config);

void ConfigInitializeCgroups(wsl::linux::WslDistributionConfig& Config);

int ConfigInitializeInstance(wsl::shared::SocketChannel& Channel, gsl::span<gsl::byte> Buffer, wsl::linux::WslDistributionConfig& Config);

void ConfigMountDrvFsVolumes(unsigned int DrvFsVolumes, uid_t OwnerUid, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config);

void ConfigMountFsTab(bool Elevated);

int ConfigReconfigureResolvConfSymlink(const wsl::linux::WslDistributionConfig& Config);

int ConfigRegisterBinfmtInterpreter(void);

int ConfigSetMountNamespace(bool Elevated);

int ConfigRemountDrvFs(gsl::span<gsl::byte> Buffer, wsl::shared::SocketChannel& Channel, const wsl::linux::WslDistributionConfig& Config);

int ConfigRemountDrvFsImpl(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config);

void ConfigUpdateLanguage(EnvironmentBlock& Environment);

void ConfigUpdateNetworkInformation(gsl::span<gsl::byte> Buffer, const wsl::linux::WslDistributionConfig& Config);

template <>
struct std::formatter<INIT_STARTUP_TYPE, char>
{
    template <typename TCtx>
    constexpr auto parse(TCtx& ctx)
    {
        return ctx.begin();
    }

    template <typename TCtx>
    auto format(INIT_STARTUP_TYPE str, TCtx& ctx) const
    {
        return std::format_to(ctx.out(), "{}", static_cast<int>(str));
    }
};