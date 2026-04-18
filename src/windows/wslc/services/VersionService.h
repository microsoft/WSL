/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionService.h

Abstract:

    This file contains the VersionService definition
--*/
#pragma once

#include "VersionModel.h"
#include <wslc.h>

namespace wsl::windows::wslc::services {
struct VersionService
{
    static std::wstring GetVersionString();
    static const wsl::windows::wslc::models::VersionInfo& VersionInfo();
};
} // namespace wsl::windows::wslc::services
