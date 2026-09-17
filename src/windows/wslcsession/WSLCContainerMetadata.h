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

    // Non-empty when the mount target is a single file rather than a directory.
    std::wstring SourceFilename;
    bool CreateSourceIfMissing{true};

    // Runtime-only field. Not serialized to JSON.
    bool Mounted{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCVolumeMount, HostPath, ParentVMPath, ContainerPath, ReadOnly, SourceFilename, CreateSourceIfMissing);
};

inline std::string_view RestartPolicyName(WSLCContainerRestartPolicy policy) noexcept
{
    switch (policy)
    {
    case WSLCContainerRestartPolicyNone:
        return "no";
    case WSLCContainerRestartPolicyAlways:
        return "always";
    case WSLCContainerRestartPolicyOnFailure:
        return "on-failure";
    case WSLCContainerRestartPolicyUnlessStopped:
        return "unless-stopped";
    default:
        return {};
    }
}

inline WSLCContainerRestartPolicy RestartPolicyFromName(std::string_view name) noexcept
{
    if (name == "no")
    {
        return WSLCContainerRestartPolicyNone;
    }
    if (name == "always")
    {
        return WSLCContainerRestartPolicyAlways;
    }
    if (name == "on-failure")
    {
        return WSLCContainerRestartPolicyOnFailure;
    }
    if (name == "unless-stopped")
    {
        return WSLCContainerRestartPolicyUnlessStopped;
    }

    return WSLCContainerRestartPolicyInvalid;
}

struct WSLCContainerRestartPolicyConfig
{
    WSLCContainerRestartPolicy Name{WSLCContainerRestartPolicyNone};
    std::int64_t MaximumRetryCount{};
};

inline void to_json(nlohmann::json& json, const WSLCContainerRestartPolicyConfig& policy)
{
    json = {
        {"Name", std::string{RestartPolicyName(policy.Name)}},
        {"MaximumRetryCount", policy.MaximumRetryCount},
    };
}

inline void from_json(const nlohmann::json& json, WSLCContainerRestartPolicyConfig& policy)
{
    policy.Name = RestartPolicyFromName(json.value("Name", std::string{"no"}));
    policy.MaximumRetryCount = json.value("MaximumRetryCount", std::int64_t{});
}

struct WSLCContainerMetadataV1
{
    WSLCContainerFlags Flags{WSLCContainerFlagsNone};
    WSLCProcessFlags InitProcessFlags{WSLCProcessFlagsNone};
    std::vector<WSLCPortMapping> Ports;
    std::vector<WSLCVolumeMount> Volumes;
    WSLCContainerRestartPolicyConfig RestartPolicy;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerMetadataV1, Flags, InitProcessFlags, Ports, Volumes, RestartPolicy);
};

struct WSLCContainerMetadata
{
    std::optional<WSLCContainerMetadataV1> V1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerMetadata, V1);
};

struct WSLCContainerRestartStateV1
{
    std::int64_t RestartCount{};
    bool HasBeenManuallyStopped{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerRestartStateV1, RestartCount, HasBeenManuallyStopped);
};

struct WSLCContainerRestartState
{
    std::optional<WSLCContainerRestartStateV1> V1;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WSLCContainerRestartState, V1);
};

} // namespace wsl::windows::service::wslc
