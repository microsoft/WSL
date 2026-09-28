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

struct CreateVolumeOptions
{
    std::string Name;
    std::optional<std::string> Driver;
    std::vector<std::pair<std::string, std::string>> DriverOpts{};
    std::vector<std::pair<std::string, std::string>> Labels{};
};

struct PruneVolumesResult
{
    std::vector<std::string> PrunedVolumes;
    ULONGLONG SpaceReclaimed{};
};

// The shape emitted by "volume list --format json"; every value is reported as a string.
struct VolumeOutputInformation
{
    std::string Availability;
    std::string Driver;
    std::string Group;
    std::string Labels;
    std::string Links;
    std::string Mountpoint;
    std::string Name;
    std::string Scope;
    std::string Size;
    std::string Status;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VolumeOutputInformation, Availability, Driver, Group, Labels, Links, Mountpoint, Name, Scope, Size, Status);
};

} // namespace wsl::windows::wslc::models
