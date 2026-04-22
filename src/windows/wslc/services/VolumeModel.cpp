/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeModel.cpp

Abstract:

    This file contains the VolumeModel implementations

--*/
#include "precomp.h"
#include "VolumeModel.h"

using namespace wsl::windows::common::string;

namespace wsl::windows::wslc::models {

Label Label::Parse(const std::wstring& value)
{
    Label result{};
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        result.m_key = WideToMultiByte(value);
    }
    else
    {
        result.m_key = WideToMultiByte(value.substr(0, pos));
        result.m_value = WideToMultiByte(value.substr(pos + 1));
    }

    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::WSLCCLI_LabelKeyEmptyError(), result.m_key.empty());
    return result;
}

DriverOption DriverOption::Parse(const std::wstring& value)
{
    DriverOption result{};
    auto pos = value.find('=');
    if (pos == std::wstring::npos)
    {
        result.m_key = WideToMultiByte(value);
        return result;
    }

    result.m_key = WideToMultiByte(value.substr(0, pos));
    result.m_value = WideToMultiByte(value.substr(pos + 1));
    return result;
}

} // namespace wsl::windows::wslc::models
