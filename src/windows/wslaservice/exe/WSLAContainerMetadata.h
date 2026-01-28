/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainerMetadata.h

Abstract:

    JSON schema for WSLA container metadata stored in Docker container labels.
    This metadata allows WSLA to recover container state across service restarts.

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::service::wsla {

// Label key used to store WSLA container metadata in Docker container labels.
constexpr auto WSLAContainerMetadataLabel = "com.microsoft.wsl.container.metadata";

struct WSLAPortMapping
{
    uint16_t HostPort{};
    uint16_t VmPort{};
    uint16_t ContainerPort{};
    int Family{};

    // Runtime-only field. Not serialized to JSON.
    bool MappedToHost{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAPortMapping, HostPort, VmPort, ContainerPort, Family);
};

struct WSLAVolumeMount
{
    std::wstring HostPath;
    std::string ParentVMPath;
    std::string ContainerPath;
    bool ReadOnly{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAVolumeMount, HostPath, ParentVMPath, ContainerPath, ReadOnly);
};

struct WSLAContainerMetadataV1
{
    bool Tty{};
    std::vector<WSLAPortMapping> Ports;
    std::vector<WSLAVolumeMount> Volumes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAContainerMetadataV1, Tty, Ports, Volumes);
};

struct WSLAContainerMetadata
{
    std::optional<WSLAContainerMetadataV1> V1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAContainerMetadata, V1);
};

} // namespace wsl::windows::service::wsla