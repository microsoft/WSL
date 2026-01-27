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
constexpr auto WSLAContainerMetadataLabel = "WSLAContainerMetadata";

// Current version of the metadata schema.
constexpr uint32_t WSLAContainerMetadataVersion = 1;

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
    std::string HostPath;
    std::string ParentVMPath;
    std::string ContainerPath;
    bool ReadOnly{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLAVolumeMount, HostPath, ParentVMPath, ContainerPath, ReadOnly);
};

struct ContainerMetadata
{
    uint32_t Version{WSLAContainerMetadataVersion};
    bool Tty{};
    std::vector<WSLAPortMapping> Ports;
    std::vector<WSLAVolumeMount> Volumes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerMetadata, Version, Tty, Ports, Volumes);
};

} // namespace wsl::windows::service::wsla