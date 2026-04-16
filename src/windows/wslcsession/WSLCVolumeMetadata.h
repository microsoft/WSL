/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumeMetadata.h

Abstract:

    JSON schema for WSLC volume metadata stored in Docker volume labels.

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::service::wslc {

// Label key used to store WSLC volume metadata in Docker volume labels.
constexpr auto WSLCVolumeMetadataLabel = "com.microsoft.wsl.volume.metadata";

// Volume driver name for VHD-backed volumes.
constexpr auto WSLCVhdVolumeDriver = "vhd";

struct WSLCVolumeMetadata
{
    std::string Driver;
    std::map<std::string, std::string> DriverOpts;
    std::map<std::string, std::string> Properties;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVolumeMetadata, Driver, DriverOpts, Properties);
};

} // namespace wsl::windows::service::wslc