/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeModel.cpp

Abstract:

    This file contains the VolumeModel implementations

--*/
#include "precomp.h"
#include "VolumeModel.h"

using namespace wsl::shared;
using namespace wsl::windows::common::string;

namespace wsl::windows::wslc::models {

std::pair<std::string, std::string> Label::Parse(const std::wstring& value)
{
    std::pair<std::string, std::string> result{};
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        result.first = WideToMultiByte(value);
    }
    else
    {
        result.first = WideToMultiByte(value.substr(0, pos));
        result.second = WideToMultiByte(value.substr(pos + 1));
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_LabelKeyEmptyError(), result.first.empty());
    return result;
}

std::pair<std::string, std::string> DriverOption::Parse(const std::wstring& value)
{
    std::pair<std::string, std::string> result{};
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        result.first = WideToMultiByte(value);
        return result;
    }

    result.first = WideToMultiByte(value.substr(0, pos));
    result.second = WideToMultiByte(value.substr(pos + 1));
    return result;
}

} // namespace wsl::windows::wslc::models
