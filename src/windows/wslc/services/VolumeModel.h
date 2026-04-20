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

struct VolumeInformation
{
    std::string Name;
    std::string Driver;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(VolumeInformation, Name, Driver);
};

struct CreateVolumeOptions
{
    std::string Name;
    std::string Driver;
    std::vector<std::pair<std::string, std::string>> DriverOpts{};
    std::vector<std::pair<std::string, std::string>> Labels{};
};

} // namespace wsl::windows::wslc::models
