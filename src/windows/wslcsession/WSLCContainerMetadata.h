/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainerMetadata.h

Abstract:

    JSON schema for WSLC container metadata stored in Docker container labels.
    This metadata allows WSLC to recover container state across service restarts.

--*/

#pragma once

#include "JsonUtils.h"
#include "wslc.h"

namespace wsl::windows::service::wslc {

// Label key used to store WSLC container metadata in Docker container labels.
constexpr auto WSLCContainerMetadataLabel = "com.microsoft.wsl.container.metadata";

struct WSLCPortMapping
{
    uint16_t HostPort{};
    uint16_t VmPort{};
    uint16_t ContainerPort{};
    int Family{};
    int Protocol{};
    std::string BindingAddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCPortMapping, HostPort, VmPort, ContainerPort, Family, Protocol, BindingAddress);
};

struct WSLCVolumeMount
{
    std::wstring HostPath;
    std::string ParentVMPath;
    std::string ContainerPath;
    bool ReadOnly{};

    // Runtime-only field. Not serialized to JSON.
    bool Mounted{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVolumeMount, HostPath, ParentVMPath, ContainerPath, ReadOnly);
};

struct WSLCContainerMetadataV1
{
    WSLCContainerFlags Flags{WSLCContainerFlagsNone};
    WSLCProcessFlags InitProcessFlags{WSLCProcessFlagsNone};
    std::vector<WSLCPortMapping> Ports;
    std::vector<WSLCVolumeMount> Volumes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerMetadataV1, Flags, InitProcessFlags, Ports, Volumes);
};

struct WSLCContainerMetadata
{
    std::optional<WSLCContainerMetadataV1> V1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerMetadata, V1);
};

} // namespace wsl::windows::service::wslc