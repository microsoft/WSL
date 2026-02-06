// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include <wil/token_helpers.h>
#include <winrt/Windows.System.Profile.h>

namespace wsl::windows::wslc::util
{
    std::wstring GetOSVersion();

    bool IsRunningAsAdmin();
}
