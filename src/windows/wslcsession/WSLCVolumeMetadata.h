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

// Volume type identifier for VHD-backed volumes.
constexpr auto WSLCVhdVolumeType = "vhd";

struct WSLCVhdVolumeMetadataV1
{
    std::wstring HostPath;
    ULONGLONG SizeBytes{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVhdVolumeMetadataV1, HostPath, SizeBytes);
};

struct WSLCVhdVolumeMetadata
{
    std::optional<WSLCVhdVolumeMetadataV1> V1;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVhdVolumeMetadata, V1);
};

struct WSLCVolumeMetadata
{
    std::string Type;
    std::optional<WSLCVhdVolumeMetadata> VhdVolumeMetadata;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVolumeMetadata, Type, VhdVolumeMetadata);
};

} // namespace wsl::windows::service::wslc
