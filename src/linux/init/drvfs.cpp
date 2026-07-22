/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    drvfs.c

Abstract:

    This file contains DrvFs function definitions.

--*/

#include "common.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <mountutilcpp.h>
#include "util.h"
#include "drvfs.h"
#include "config.h"
#include "message.h"
#include <cassert>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

#define PLAN9_CASE_OPTION "case="
#define PLAN9_CASE_OPTION_DIR PLAN9_CASE_OPTION "dir"
#define PLAN9_CASE_OPTION_FORCE PLAN9_CASE_OPTION "force"
#define PLAN9_CASE_OPTION_OFF PLAN9_CASE_OPTION "off"
#define PLAN9_SYMLINK_ROOT_OPTION "symlinkroot="
#define PLAN9_UNC_PREFIX_LENGTH (2)

#define VIRTIOFS_TAG_DIR "/run/wsl/virtiofs"

#define LOG_STDERR(_errno) fprintf(stderr, "mount: %s\n", strerror(_errno))

constexpr int c_exitCodeInvalidUsage = 1;
constexpr int c_exitCodeMountFail = 32;

int MountFilesystem(const char* FsType, const char* Source, const char* Target, const char* Options, int* ExitCode = nullptr);

int MountWithRetry(const char* Source, const char* Target, const char* FsType, const char* Options, int* ExitCode = nullptr);

std::optional<VirtioFsMountRoot> ParseAggregateVirtioFsMountRoot(std::string_view Tag, std::string_view Root)
{
    if ((Tag != LX_INIT_DRVFS_VIRTIO_TAG && Tag != LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) || Root.size() < 2 || Root.front() != '/')
    {
        return {};
    }

    Root.remove_prefix(1);
    const auto separator = Root.find('/');
    const auto childName = Root.substr(0, separator);
    if (!wsl::shared::string::ToGuid(childName))
    {
        return {};
    }

    const auto subPath = separator == std::string_view::npos ? std::string_view{"/"} : Root.substr(separator);
    return VirtioFsMountRoot{childName, subPath};
}

namespace {
std::mutex g_virtiofsDeviceMutex;

bool IsMountPoint(const std::string& Path)
{
    struct stat self = {};
    struct stat parent = {};
    if (stat(Path.c_str(), &self) != 0)
    {
        return false;
    }

    const auto parentPath = Path + "/..";
    if (stat(parentPath.c_str(), &parent) != 0)
    {
        return false;
    }

    return self.st_dev != parent.st_dev;
}
} // namespace

int MountVirtioFsChild(
    const char* Tag,
    const char* ChildName,
    const char* Target,
    const char* Options,
    int* ExitCode = nullptr,
    std::string_view SubPath = "/")
{
    if ((strcmp(Tag, LX_INIT_DRVFS_VIRTIO_TAG) != 0 && strcmp(Tag, LX_INIT_DRVFS_ADMIN_VIRTIO_TAG) != 0) ||
        !wsl::shared::string::ToGuid(ChildName) || SubPath.empty() || SubPath.front() != '/')
    {
        errno = EINVAL;
        return -1;
    }

    const auto rootTarget = std::format("{}/{}", VIRTIOFS_MOUNT_DIR, Tag);
    const auto childSource = std::format("{}/{}{}", rootTarget, ChildName, SubPath == "/" ? std::string_view{} : SubPath);
    {
        std::lock_guard<std::mutex> lock(g_virtiofsDeviceMutex);
        if (!IsMountPoint(rootTarget))
        {
            if (UtilMkdirPath(rootTarget.c_str(), 0755) < 0)
            {
                return -1;
            }

            if (MountWithRetry(Tag, rootTarget.c_str(), VIRTIO_FS_TYPE, "", ExitCode) < 0 && !IsMountPoint(rootTarget))
            {
                return -1;
            }
        }
    }

    if (UtilMkdirPath(Target, 0755) < 0)
    {
        return -1;
    }

    constexpr int c_maxAttempts = 5;
    int lastError = 0;
    for (int attempt = 0; attempt < c_maxAttempts; ++attempt)
    {
        if (mount(childSource.c_str(), Target, nullptr, MS_BIND, nullptr) == 0)
        {
            lastError = 0;
            break;
        }

        lastError = errno;
        if (lastError != ENOENT)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    if (lastError != 0)
    {
        errno = lastError;
        if (ExitCode != nullptr)
        {
            *ExitCode = c_exitCodeMountFail;
        }

        return -1;
    }

    const auto parsed = mountutil::MountParseFlags(Options ? Options : "");
    auto bindFlags = parsed.MountFlags;
    if ((bindFlags & (MS_NOATIME | MS_STRICTATIME)) == 0)
    {
        bindFlags |= MS_RELATIME;
    }

    if (mount(nullptr, Target, nullptr, MS_BIND | MS_REMOUNT | bindFlags, parsed.StringOptions.c_str()) < 0)
    {
        const auto savedError = errno;
        // Remove the successful bind before MountVirtioFs falls back to Plan 9.
        umount2(Target, MNT_DETACH);
        errno = savedError;
        if (ExitCode != nullptr)
        {
            *ExitCode = c_exitCodeMountFail;
        }

        return -1;
    }

    if (ExitCode != nullptr)
    {
        *ExitCode = 0;
    }

    return 0;
}

void SaveVirtiofsTagMapping(const char* Tag, const char* Source)

/*++

Routine Description:

    This routine creates a symlink in VIRTIOFS_TAG_DIR that maps a virtiofs tag
    to its Windows mount source path. This allows QueryVirtiofsMountSource to
    resolve tags without talking to the service.

Arguments:

    Tag - Supplies the virtiofs tag.

    Source - Supplies the Windows path the tag refers to.

Return Value:

    None.

--*/

{
    //
    // Validate the tag is a GUID to prevent path traversal.
    //

    const auto Guid = wsl::shared::string::ToGuid(Tag);
    if (!Guid)
    {
        LOG_WARNING("Invalid virtiofs tag {}", Tag);
        return;
    }

    //
    // Canonicalize path separators to backslashes before persisting.
    //

    std::string CanonicalSource{Source};
    UtilCanonicalisePathSeparator(CanonicalSource, PATH_SEP_NT);

    UtilMkdirPath(VIRTIOFS_TAG_DIR, 0755);

    auto LinkPath = std::format("{}/{}", VIRTIOFS_TAG_DIR, Tag);

    //
    // Remove any existing symlink for this tag before creating a new one.
    //

    unlink(LinkPath.c_str());
    if (symlink(CanonicalSource.c_str(), LinkPath.c_str()) < 0)
    {
        LOG_WARNING("Failed to create virtiofs tag symlink {} -> {}: {}", LinkPath, CanonicalSource, errno);
    }
}

std::pair<std::string, std::string> ConvertDrvfsMountOptionsToPlan9(std::string_view Options, const wsl::linux::WslDistributionConfig& Config)

/*++

Routine Description:

    This routine converts each applicable DrvFs mount option into a 9p mount option and splits
    non-DrvFs options to a different list.

Arguments:

    Options - Supplies the DrvFs mount options.

Return Value:

    A pair representing the 9p mount options, and standard mount options.

--*/

{
    using wsl::shared::string::StartsWith;

    std::string Plan9Options{};
    std::string StandardOptions{};
    while (!Options.empty())
    {
        auto Option = UtilStringNextToken(Options, ",");
        if (Option == "ro")
        {
            Plan9Options += ";ro";
            StandardOptions += "ro,";
        }
        else if (
            (Option == "metadata") || (StartsWith(Option, PLAN9_CASE_OPTION)) || (StartsWith(Option, "uid=")) ||
            (StartsWith(Option, "gid=")) || (StartsWith(Option, "umask=")) || (StartsWith(Option, "dmask=")) ||
            (StartsWith(Option, "fmask=")) || (StartsWith(Option, PLAN9_SYMLINK_ROOT_OPTION)))
        {
            if (Option == PLAN9_CASE_OPTION_FORCE)
            {
                LOG_WARNING("{} not supported, using {}", PLAN9_CASE_OPTION_FORCE, PLAN9_CASE_OPTION_DIR);
                Option = PLAN9_CASE_OPTION_DIR;
            }

            Plan9Options += ';';
            Plan9Options += Option;
        }
        else if (StartsWith(Option, "fallback="))
        {
            LOG_WARNING("{} not supported, ignoring...", Option);
        }
        else
        {
            StandardOptions += Option;
            StandardOptions += ',';
        }
    }

    Plan9Options += ";" PLAN9_SYMLINK_ROOT_OPTION;
    Plan9Options += Config.DrvFsPrefix;
    return {std::move(Plan9Options), std::move(StandardOptions)};
}

bool IsDrvfsElevated(void)

/*++

Routine Description:

    This routine determines whether drvfs mounts should use the elevated server.

Arguments:

    None.

Return Value:

    True if this process should use the elevated server; otherwise, false.

--*/

{
    const char* envFlag = getenv(WSL_DRVFS_ELEVATED_ENV);
    if (envFlag != nullptr)
    {
        if (strcmp(envFlag, "0") == 0)
        {
            return false;
        }
        else if (strcmp(envFlag, "1") == 0)
        {
            return true;
        }

        LOG_ERROR("Unexpected value for {}: '{}'", WSL_DRVFS_ELEVATED_ENV, envFlag);
    }

    //
    // Establish a connection to the interop server. If the connection cannot
    // be established, use the non-admin DrvFs port.
    //

    wsl::shared::SocketChannel channel{UtilConnectToInteropServer(), "InteropClientDrvfs"};
    if (channel.Socket() < 0)
    {
        return false;
    }

    //
    // Query the interop server for which port to use.
    //

    MESSAGE_HEADER QueryPortMessage;
    QueryPortMessage.MessageType = LxInitMessageQueryDrvfsElevated;
    QueryPortMessage.MessageSize = sizeof(QueryPortMessage);

    auto transaction = channel.StartTransaction();
    transaction.Send(QueryPortMessage);
    return transaction.Receive<RESULT_MESSAGE<bool>>().Result;
}

int MountFilesystem(const char* FsType, const char* Source, const char* Target, const char* Options, int* ExitCode)

/*++

Routine Description:

    This routine will perform a mount using the /bin/mount binary.

Arguments:

    FsType - Supplies the file system type.

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies the mount options.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

{
    const char* const Argv[] = {
        MOUNT_COMMAND, MOUNT_INTERNAL_ONLY_ARG, MOUNT_TYPES_ARG, FsType, Source, Target, MOUNT_OPTIONS_ARG, Options, nullptr};

    int Status = 0;
    const int Result = UtilCreateProcessAndWait(Argv[0], Argv, &Status);

    //
    // If the mount process failed, make sure its exit code is propagated. If it terminated
    // abnormally or could not be launched, just return failure.
    //

    if (ExitCode != nullptr)
    {
        if (Result < 0)
        {
            if (WIFEXITED(Status) && Status != 0)
            {
                *ExitCode = WEXITSTATUS(Status);
            }
            else
            {
                *ExitCode = c_exitCodeMountFail;
            }
        }
        else
        {
            *ExitCode = 0;
        }
    }

    return Result;
}

int MountWithRetry(const char* Source, const char* Target, const char* FsType, const char* Options, int* ExitCode)

/*++

Routine Description:

    This routine performs a mount with retry logic for DrvFs filesystems.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    FsType - Supplies the filesystem type.

    Options - Supplies the mount options.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // Verify the target directory exists before mounting.
    //

    int Result = access(Target, F_OK);
    if (Result < 0)
    {
        LOG_STDERR(errno);
    }
    else
    {
        auto Parsed = mountutil::MountParseFlags(Options);
        Result = UtilMount(Source, Target, FsType, Parsed.MountFlags, Parsed.StringOptions.c_str(), std::chrono::seconds{2});
    }

    if (ExitCode)
    {
        *ExitCode = Result < 0 ? c_exitCodeMountFail : 0;
    }

    return Result;
}
CATCH_RETURN_ERRNO()

int MountDrvfs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode)

/*++

Routine Description:

    This routine will perform a DrvFs mount.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies the mount options.

    Admin - Supplies an optional boolean to specify if the admin or non-admin share should be used.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    if (!UtilIsUtilityVm())
    {
        return MountFilesystem(DRVFS_FS_TYPE, Source, Target, Options, ExitCode);
    }
    else if (WSL_USE_VIRTIO_FS())
    {
        return MountVirtioFs(Source, Target, Options, Admin, Config, ExitCode);
    }

    return MountPlan9(Source, Target, Options, Admin, Config, ExitCode);
}
CATCH_RETURN_ERRNO()

int MountDrvfsEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the entrypoint for mount.drvfs.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    0 on success, -1 on failure.

--*/

{
    if (Argc < 3)
    {
        LOG_STDERR(EINVAL);
        return c_exitCodeInvalidUsage;
    }

    //
    // Handle mount options if provided.
    //

    auto* Options = "";
    if (Argc > 4)
    {
        Options = Argv[4];
    }

    int ExitCode = c_exitCodeMountFail;
    MountDrvfs(Argv[1], Argv[2], Options, {}, wsl::linux::WslDistributionConfig{CONFIG_FILE}, &ExitCode);
    return ExitCode;
}

int MountPlan9Share(const char* Source, const char* Target, const char* Options, bool Admin, int* ExitCode)

/*++

Routine Description:

    This routine will perform a plan 9 mount using the /bin/mount binary.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies the mount options.

    Admin - Supplies a boolean specifying if the admin share should be used.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

{
    std::string MountOptions;
    if (WSL_USE_VIRTIO_9P())
    {
        Source = Admin ? LX_INIT_DRVFS_ADMIN_VIRTIO_TAG : LX_INIT_DRVFS_VIRTIO_TAG;
        MountOptions = std::format("msize=262144,trans=virtio,{}", Options);
        return MountWithRetry(Source, Target, PLAN9_FS_TYPE, MountOptions.c_str(), ExitCode);
    }
    else
    {
        auto Port = Admin ? LX_INIT_UTILITY_VM_PLAN9_DRVFS_ADMIN_PORT : LX_INIT_UTILITY_VM_PLAN9_DRVFS_PORT;
        wil::unique_fd Fd{UtilConnectVsock(Port, false, LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE)};
        if (!Fd)
        {
            return -1;
        }

        MountOptions =
            std::format("msize={},trans=fd,rfdno={},wfdno={},{}", LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE, Fd.get(), Fd.get(), Options);

        return MountFilesystem(PLAN9_FS_TYPE, Source, Target, MountOptions.c_str(), ExitCode);
    }
}

int MountPlan9(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode)

/*++

Routine Description:

    This routine will perform a DrvFs mount using Plan9.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies the mount options.

    Admin - Supplies an optional boolean to specify if the admin or non-admin share should be used.

    Config - Supplies the distribution configuration.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    //
    // Check if the path is a UNC path.
    //

    const char* Plan9Source;
    std::string UncSource;
    if ((strlen(Source) >= PLAN9_UNC_PREFIX_LENGTH) && ((Source[0] == '/') || (Source[0] == '\\')) &&
        ((Source[1] == '/') || (Source[1] == '\\')))
    {
        UncSource = PLAN9_UNC_TRANSLATED_PREFIX;
        UncSource += &Source[PLAN9_UNC_PREFIX_LENGTH];
        Plan9Source = UncSource.c_str();
    }
    else
    {
        Plan9Source = Source;
    }

    //
    // Check whether to use the elevated or regular 9p server.
    //

    bool Elevated = Admin.has_value() ? Admin.value() : IsDrvfsElevated();

    //
    // Initialize mount options.
    //

    auto Plan9Options = std::format("{};path={}", PLAN9_ANAME_DRVFS, Plan9Source);

    //
    // N.B. The cache option is added to the start of this so if the user
    //      specifies one explicitly, it will override the default.
    //

    std::string MountOptions = "cache=mmap,";
    auto ParsedOptions = ConvertDrvfsMountOptionsToPlan9(Options ? Options : "", Config);
    Plan9Options += ParsedOptions.first;
    MountOptions += ParsedOptions.second;

    //
    // Append the 9p mount options to the end of the other mount options and perform the mount operation.
    //

    MountOptions += Plan9Options;
    if (MountPlan9Share(Source, Target, MountOptions.c_str(), Elevated, ExitCode) < 0)
    {
        return -1;
    }

    return 0;
}
CATCH_RETURN_ERRNO()

int MountVirtioFs(const char* Source, const char* Target, const char* Options, std::optional<bool> Admin, const wsl::linux::WslDistributionConfig& Config, int* ExitCode)

/*++

Routine Description:

    This routine mounts a virtiofs share. The DrvFs mount options are converted into 9p mount options
    which are used to determine behavior when the device is added to the host.

Arguments:

    Source - Supplies the mount source.

    Target - Supplies the mount target.

    Options - Supplies DrvFs mount options to translate into Plan9 mount
        options.

    Admin - Supplies an optional boolean to specify if the admin or non-admin server should be used.

    ExitCode - Supplies an optional pointer that receives the exit code.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    assert(WSL_USE_VIRTIO_FS());

    //
    // Check whether to use the elevated or non-elevated virtiofs server.
    //

    if (!Admin.has_value())
    {
        Admin = IsDrvfsElevated();
    }

    //
    // Convert the DrvFs mount options.
    //
    // N.B. Since virtiofs does not allow passing mount options, the 9p mount options are used to specify share
    //      behavior when creating virtiofs shares on the host.
    //

    auto [Plan9Options, MountOptions] = ConvertDrvfsMountOptionsToPlan9(Options ? Options : "", Config);

    //
    // Construct a request to add a virtiofs share.
    //

    wsl::shared::MessageWriter<LX_INIT_ADD_VIRTIOFS_SHARE_MESSAGE> AddShare(LxInitMessageAddVirtioFsDevice);
    AddShare->Admin = Admin.value();
    AddShare.WriteString(AddShare->PathOffset, Source);
    AddShare.WriteString(AddShare->OptionsOffset, Plan9Options);

    //
    // Connect to the wsl service to add the virtiofs share. If adding the share fails, fallback to mounting using Plan9.
    //

    wsl::shared::SocketChannel Channel{UtilConnectVsock(LX_INIT_UTILITY_VM_VIRTIOFS_PORT, true), "VirtoFs"};
    if (Channel.Socket() < 0)
    {
        return -1;
    }

    gsl::span<gsl::byte> ResponseSpan;
    const auto& Response = Channel.Transaction<LX_INIT_ADD_VIRTIOFS_SHARE_MESSAGE>(AddShare.Span(), &ResponseSpan);
    if (Response.Result != 0)
    {
        LOG_WARNING("Add virtiofs share for {} failed {}, falling back to Plan9", Source, Response.Result);
        return MountPlan9(Source, Target, Options, Admin, Config, ExitCode);
    }

    //
    // Perform the mount operation.
    //

    auto* Tag = wsl::shared::string::FromSpan(ResponseSpan, Response.TagOffset);
    auto* ChildName = wsl::shared::string::FromSpan(ResponseSpan, Response.ChildNameOffset);
    auto* ResponseSource = wsl::shared::string::FromSpan(ResponseSpan, Response.SourceOffset);
    const char* MappingName = Tag;
    if (*ChildName == '\0')
    {
        THROW_LAST_ERROR_IF(MountWithRetry(Tag, Target, VIRTIO_FS_TYPE, MountOptions.c_str(), ExitCode) < 0);
    }
    else if (MountVirtioFsChild(Tag, ChildName, Target, MountOptions.c_str(), ExitCode) < 0)
    {
        const auto childError = errno;
        LOG_WARNING("Mounting virtiofs child for {} failed {}, falling back to Plan9", Source, childError);

        return MountPlan9(Source, Target, Options, Admin, Config, ExitCode);
    }
    else
    {
        MappingName = ChildName;
    }

    //
    // Save the tag mapping.
    //
    // N.B. Use the source path from the response since the service canonicalizes it.
    //

    SaveVirtiofsTagMapping(MappingName, ResponseSource);

    return 0;
}
CATCH_RETURN_ERRNO()

int RemountVirtioFs(const char* Tag, const char* Target, const char* Options, bool Admin, std::string_view SubPath)

/*++

Routine Description:

    This routine translates DrvFs mount options into Plan9 mount options and
    mounts the share.

Arguments:

    Tag - Supplies the virtiofs tag to remount.

    Target - Supplies the mount target.

    Options - Supplies mount options.

    Admin - Supplies a boolean to specify if the admin or non-admin server should be used.

Return Value:

    0 on success, -1 on failure.

--*/

try
{
    assert(WSL_USE_VIRTIO_FS());

    wsl::shared::MessageWriter<LX_INIT_REMOUNT_VIRTIOFS_SHARE_MESSAGE> RemountShare(LxInitMessageRemountVirtioFsDevice);
    RemountShare->Admin = Admin;
    RemountShare.WriteString(RemountShare->TagOffset, Tag);

    //
    // Connect to the host and send the remount request.
    //

    wsl::shared::SocketChannel Channel{UtilConnectVsock(LX_INIT_UTILITY_VM_VIRTIOFS_PORT, true), "RemountVirtioFs"};
    if (Channel.Socket() < 0)
    {
        return -1;
    }

    gsl::span<gsl::byte> ResponseSpan;
    const auto& Response = Channel.Transaction<LX_INIT_REMOUNT_VIRTIOFS_SHARE_MESSAGE>(RemountShare.Span(), &ResponseSpan);
    if (Response.Result != 0)
    {
        LOG_ERROR("Remount virtiofs share for {} failed {}", Tag, Response.Result);
        return -1;
    }

    auto* NewTag = wsl::shared::string::FromSpan(ResponseSpan, Response.TagOffset);
    auto* ChildName = wsl::shared::string::FromSpan(ResponseSpan, Response.ChildNameOffset);
    auto* Source = wsl::shared::string::FromSpan(ResponseSpan, Response.SourceOffset);
    const char* MappingName = NewTag;
    if (*ChildName == '\0')
    {
        THROW_LAST_ERROR_IF(MountWithRetry(NewTag, Target, VIRTIO_FS_TYPE, Options) < 0);
    }
    else
    {
        THROW_LAST_ERROR_IF(MountVirtioFsChild(NewTag, ChildName, Target, Options, nullptr, SubPath) < 0);
        MappingName = ChildName;
    }

    SaveVirtiofsTagMapping(MappingName, Source);

    return 0;
}
CATCH_RETURN_ERRNO()

std::string QueryVirtiofsMountSource(const char* Tag, const char* Root)

/*++

Routine Description:

    This routine takes a virtiofs tag and determines the Windows path it refers to
    by reading the symlink created during mount.

Arguments:

    Tag - Supplies the virtiofs tag to query.

    Root - Optionally supplies the mountinfo root for an aggregate child.

Return Value:

    The mount source, an empty string on failure.

--*/

try
{
    if (!WSL_USE_VIRTIO_FS())
    {
        return {};
    }

    //
    // Validate the tag is a GUID.
    //

    std::string mappingName{Tag};
    if (Root != nullptr)
    {
        if (const auto mountRoot = ParseAggregateVirtioFsMountRoot(Tag, Root))
        {
            mappingName = mountRoot->ChildName;
        }
    }

    const auto Guid = wsl::shared::string::ToGuid(mappingName);
    if (!Guid)
    {
        return {};
    }

    //
    // Read the symlink that maps this tag to its Windows source path.
    //

    auto LinkPath = std::format("{}/{}", VIRTIOFS_TAG_DIR, mappingName);
    return std::filesystem::read_symlink(LinkPath).string();
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}
