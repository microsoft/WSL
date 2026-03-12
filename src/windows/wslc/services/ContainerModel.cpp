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

using namespace wsl::shared::string;

VolumeMount VolumeMount::Parse(const std::wstring& value)
{
    auto lastColon = value.rfind(':');
    if (lastColon == std::wstring::npos)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount value must be in the format <host path>:<container path>[:mode]");
    }

    VolumeMount vm;
    auto splitColon = lastColon;
    const auto lastToken = value.substr(lastColon + 1);
    if (IsValidMode(lastToken))
    {
        vm.m_isReadOnlyMode = IsReadOnlyMode(lastToken);
        if (lastColon == 0)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount value must be in the format <host path>:<container path>[:mode]");
        }

        splitColon = value.rfind(':', lastColon - 1);
        if (splitColon == std::string::npos)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount value must be in the format <host path>:<container path>[:mode]");
        }

        vm.m_containerPath = WideToMultiByte(value.substr(splitColon + 1, lastColon - splitColon - 1));
    }
    else
    {
        vm.m_containerPath = WideToMultiByte(lastToken);
    }

    vm.m_hostPath = value.substr(0, splitColon);
    if (vm.m_hostPath.empty() || vm.m_containerPath.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, "Volume mount value must be in the format <host path>:<container path>[:mode]");
    }

    return vm;
}
} // namespace wsl::windows::wslc::models