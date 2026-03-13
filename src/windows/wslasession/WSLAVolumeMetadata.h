/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVolumeMetadata.h

Abstract:

    JSON schema for WSLA volume metadata stored in Docker volume labels.

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::service::wsla {

// Label key used to store WSLA volume metadata in Docker volume labels.
constexpr auto WSLAVolumeMetadataLabel = "com.microsoft.wsl.volume.metadata";

struct WSLAVhdVolumeMetadataV1
{
    std::wstring HostPath;
    ULONGLONG SizeBytes{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAVhdVolumeMetadataV1, HostPath, SizeBytes);
};

struct WSLAVhdVolumeMetadata
{
    std::optional<WSLAVhdVolumeMetadataV1> V1;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAVhdVolumeMetadata, V1);
};

struct WSLAVolumeMetadata
{
    std::string Type;
    std::optional<WSLAVhdVolumeMetadata> VhdVolumeMetadata;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAVolumeMetadata, Type, VhdVolumeMetadata);
};

} // namespace wsl::windows::service::wsla
