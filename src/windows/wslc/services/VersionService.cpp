/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionService.cpp

Abstract:

    This file contains the VersionService implementation
--*/

#include "precomp.h"
#include "VersionService.h"
#include "Command.h"

namespace wsl::windows::wslc::services {

using namespace wsl::shared::string;

std::wstring VersionService::GetVersionString()
{
    return std::format(L"{} {}", s_ExecutableName, MultiByteToWide(VersionInfo().Client.Version));
}

const wsl::windows::wslc::models::VersionInfo& VersionService::VersionInfo()
{
    static const wsl::windows::wslc::models::VersionInfo s_versionInfo{};
    return s_versionInfo;
}

} // namespace wsl::windows::wslc::services
