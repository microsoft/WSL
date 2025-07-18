// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

extern "C" {
#include "mountutil.h"
}

namespace mountutil {

// C++ wrapper for the MOUNT_ENUM structure.
class MountEnum
{
public:
    // Initialize a new instance of the MountEnum class.
    MountEnum(const char* mountInfoFile = MOUNT_INFO_FILE)
    {
        THROW_LAST_ERROR_IF(MountEnumCreateEx(&m_mountEnum, mountInfoFile) < 0);
    }

    // Destruct this instance of the MountEnum class.
    ~MountEnum()
    {
        MountEnumFree(&m_mountEnum);
    }

    MountEnum(const MountEnum&) = delete;
    MountEnum& operator=(const MountEnum&) = delete;

    // Get the next entry in the mountinfo file. Returns false if the end is reached.
    bool Next()
    {
        if (MountEnumNext(&m_mountEnum) < 0)
        {
            if (errno == 0)
            {
                return false;
            }

            THROW_ERRNO(errno);
        }

        return true;
    }

    // Return the current entry.
    // N.B. You must call Next at least once before this is valid.
    // N.B. The strings in the current entry are valid only until Next is called again or until this
    //      class is destructed.
    MOUNT_ENTRY& Current()
    {
        return m_mountEnum.Current;
    }

    // Finds a mount using the specified predicate. Returns false if there is no matching entry.
    // N.B. If the function returns true, use Current to get the matching entry.
    bool FindMount(const std::function<bool(const MOUNT_ENTRY&)>& predicate)
    {
        while (Next())
        {
            if (predicate(m_mountEnum.Current))
            {
                return true;
            }
        }

        return false;
    }

private:
    MOUNT_ENUM m_mountEnum{};
};

struct ParsedOptions
{
    std::string StringOptions;
    int MountFlags;
    bool NoFail;
};

ParsedOptions MountParseFlags(std::string_view options);

int MountFilesystem(const char* source, const char* target, const char* type, const char* options);

} // namespace mountutil
