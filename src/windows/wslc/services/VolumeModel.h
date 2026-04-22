/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeModel.h

Abstract:

    This file contains the VolumeModel definitions

--*/

#pragma once

#include "JsonUtils.h"
#include <string>

namespace wsl::windows::wslc::models {

struct Label
{
    std::string Key() const
    {
        return m_key;
    }

    std::string Value() const
    {
        return m_value;
    }

    static Label Parse(const std::wstring& value);

private:
    std::string m_key;
    std::string m_value;
};

struct DriverOption
{
    std::string Key() const
    {
        return m_key;
    }

    std::string Value() const
    {
        return m_value;
    }

    static DriverOption Parse(const std::wstring& value);

private:
    std::string m_key;
    std::string m_value;
};

struct CreateVolumeOptions
{
    std::string Name;
    std::optional<std::string> Driver;
    std::vector<std::pair<std::string, std::string>> DriverOpts{};
    std::vector<std::pair<std::string, std::string>> Labels{};
};

} // namespace wsl::windows::wslc::models
