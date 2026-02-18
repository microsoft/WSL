/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerModel.cpp

Abstract:

    This file contains the ContainerModel implementation

--*/

#include <precomp.h>
#include "ContainerModel.h"

namespace wsl::windows::wslc::models {
static bool IsWindowsDriveColon(const std::string& s, size_t index, size_t tokenStart) {
    if (s[index] != ':' || index != tokenStart + 1 || !std::isalpha((unsigned char)s[tokenStart]) || index + 1 >= s.size())
    {
        return false;
    }

    return s[index + 1] == '\\' || s[index + 1] == '/';
}

static std::vector<std::string> SplitVolumeValue(const std::string& value)
{
    std::vector<std::string> parts;
    std::string current;
    size_t tokenStart = 0;

    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == ':')
        {
            if (IsWindowsDriveColon(value, i, tokenStart))
            {
                current.push_back(c);
            }
            else
            {
                parts.push_back(current);
                current.clear();
                tokenStart = i + 1;
            }
        }
        else
        {
            current.push_back(c);
        }
    }

    parts.push_back(current);
    return parts;
}

VolumeMount VolumeMount::Parse(const std::string& value)
{
    auto parts = SplitVolumeValue(value);
    if (parts.size() < 2 || parts.size() > 3)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount value must be in the format <host path>:<container path>[:mode]");
    }

    VolumeMount vm;
    vm.m_hostPath = parts[0];
    vm.m_containerPath = parts[1];
    if (parts.size() == 3)
    {
        vm.m_mode = parts[2];
        if (vm.m_mode != "ro" && vm.m_mode != "rw")
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount mode must be either 'ro' or 'rw'");
        }
    }

    return vm;
}
}