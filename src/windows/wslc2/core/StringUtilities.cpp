// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "StringUtilities.h"
 
namespace wsl::windows::wslc::util
{
    std::wstring ToLower(std::wstring_view in)
    {
        std::wstring result(in);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned short c) { return std::towlower(c); });
        return result;
    }
}