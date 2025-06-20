// Copyright (C) Microsoft Corporation. All rights reserved.
#include <string>
#include <string_view>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <lxdef.h>
#include <lxwil.h>
#include "mountutilcpp.h"

namespace {

enum class ParseFlags
{
    None = 0,
    Remove = 0x1,
    NoFail = 0x2,
    OptionalValue = 0x4,
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
DEFINE_ENUM_FLAG_OPERATORS(ParseFlags);
#pragma clang diagnostic pop

struct MountFlag
{
    const char* Name;
    int MountFlags;
    ParseFlags ParseFlags;
};

#define FLAG_WITH_NAMED_INVERSE(name, inverse, flag) \
    {(name), (flag), ParseFlags::None}, \
    { \
        (inverse), (flag), ParseFlags::Remove \
    }

// "opt", "noopt" pair where the "noopt" version adds a flag, and "opt" removes it.
#define NO_FLAG_WITH_INVERSE(name, flag) FLAG_WITH_NAMED_INVERSE("no" name, name, flag)

// "opt", "noopt" pair where the "opt" version adds a flag, and "noopt" removes it.
#define FLAG_WITH_INVERSE(name, flag) FLAG_WITH_NAMED_INVERSE(name, "no" name, flag)

// List of mount options that translate into mount flags.
// This is based on the information in the mount(8) manpage. Note that not all options are present,
// since this is intended to be used by a mount helper, and not all options are forwarded to the
// helpers by /bin/mount.
const MountFlag c_flagMap[] = {
    FLAG_WITH_NAMED_INVERSE("sync", "async", MS_SYNCHRONOUS),
    NO_FLAG_WITH_INVERSE("atime", MS_NOATIME),
    {"defaults", 0, ParseFlags::None},
    NO_FLAG_WITH_INVERSE("dev", MS_NODEV),
    NO_FLAG_WITH_INVERSE("diratime", MS_NODIRATIME),
    {"dirsync", MS_DIRSYNC, ParseFlags::None},
    NO_FLAG_WITH_INVERSE("exec", MS_NOEXEC),
    {"group", MS_NOSUID | MS_NODEV, ParseFlags::None},
    {"nogroup", 0, ParseFlags::None},
    FLAG_WITH_INVERSE("iversion", MS_I_VERSION),
    FLAG_WITH_INVERSE("mand", MS_MANDLOCK),
    {"_netdev", 0, ParseFlags::None},
    {"nofail", 0, ParseFlags::NoFail},
    FLAG_WITH_INVERSE("relatime", MS_RELATIME),
    FLAG_WITH_INVERSE("strictatime", MS_STRICTATIME),
    FLAG_WITH_INVERSE("lazytime", MS_LAZYTIME),
    NO_FLAG_WITH_INVERSE("suid", MS_NOSUID),
    FLAG_WITH_NAMED_INVERSE("silent", "loud", MS_SILENT),
    {"owner", MS_NODEV | MS_NOSUID, ParseFlags::None},
    {"noowner", 0, ParseFlags::None},
    {"remount", MS_REMOUNT, ParseFlags::None},
    FLAG_WITH_NAMED_INVERSE("ro", "rw", MS_RDONLY),
    {"user", MS_NOEXEC | MS_NODEV | MS_NOSUID, ParseFlags::OptionalValue},
    {"nouser", 0, ParseFlags::None},
    {"users", MS_NOEXEC | MS_NODEV | MS_NOSUID, ParseFlags::None},
    {"nousers", 0, ParseFlags::None},
};

// Determine if an option should be a flag.
// Returns the flag information if found; otherwise, null.
const MountFlag* FindOption(std::string_view option)
{
    // Check if the option has a value.
    bool hasValue = false;
    auto index = option.find_first_of('=');
    if (index != std::string_view::npos)
    {
        hasValue = true;
        option = option.substr(0, index);
    }

    for (auto& flag : c_flagMap)
    {
        // If the option has a value, ignore the entry if it doesn't allow one.
        if (hasValue && !WI_IsFlagSet(flag.ParseFlags, ParseFlags::OptionalValue))
        {
            continue;
        }

        if (option == flag.Name)
        {
            return &flag;
        }
    }

    return nullptr;
}

// Retrieves the next character-separated token from a string view, returning
// the token and updating the view to be the remainder of the string.
std::string_view NextToken(std::string_view& view, char separator)
{
    std::string_view result;
    auto pos = view.find_first_of(separator);
    if (pos == view.npos)
    {
        result = view;
        view = {};
    }
    else
    {
        result = view.substr(0, pos);
        view = view.substr(pos + 1);
    }

    return result;
}

} // namespace

namespace mountutil {

ParsedOptions MountParseFlags(std::string_view options)
{
    ParsedOptions result{};
    while (!options.empty())
    {
        // Get the next option and check if it's a flag.
        auto option = NextToken(options, ',');
        auto flag = FindOption(option);
        if (flag == nullptr)
        {
            // Not a flag, so append to the string options.
            if (!result.StringOptions.empty())
            {
                result.StringOptions += ',';
            }

            result.StringOptions += option;
        }
        else
        {
            // Modify the mount flags.
            if (WI_IsFlagSet(flag->ParseFlags, ParseFlags::Remove))
            {
                WI_ClearAllFlags(result.MountFlags, flag->MountFlags);
            }
            else
            {
                WI_SetAllFlags(result.MountFlags, flag->MountFlags);
            }

            if (WI_IsFlagSet(flag->ParseFlags, ParseFlags::NoFail))
            {
                result.NoFail = true;
            }
        }
    }

    return result;
}

int MountFilesystem(const char* source, const char* target, const char* type, const char* options)
{
    auto parsedOptions = MountParseFlags(options);
    int result = mount(source, target, type, parsedOptions.MountFlags, parsedOptions.StringOptions.c_str());

    // If the nofail option was specified, ENOENT on the source only must be ignored.
    if (result < 0 && errno == ENOENT && parsedOptions.NoFail)
    {
        struct stat st;

        // If the target exists, the error must be about the source, so ignore it.
        if (stat(target, &st) == 0)
        {
            return 0;
        }

        // In case stat changed errno.
        errno = ENOENT;
    }

    return result;
}

} // namespace mountutil
