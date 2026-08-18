/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    cdi_schema.h

Abstract:

    Schema for Container Device Interface (CDI) specs.
    See https://github.com/cncf-tags/container-device-interface/blob/main/SPEC.md

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::shared::cdi {

struct DeviceNode
{
    std::string path;
    std::string permissions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DeviceNode, path, permissions);
};

struct Mount
{
    std::string hostPath;
    std::string containerPath;
    std::vector<std::string> options;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Mount, hostPath, containerPath, options);
};

struct Hook
{
    std::string hookName;
    std::string path;
    std::vector<std::string> args;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Hook, hookName, path, args);
};

struct ContainerEdits
{
    std::vector<DeviceNode> deviceNodes;
    std::vector<Mount> mounts;
    std::vector<Hook> hooks;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerEdits, deviceNodes, mounts, hooks);
};

struct Device
{
    std::string name;
    ContainerEdits containerEdits;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Device, name, containerEdits);
};

struct Spec
{
    std::string cdiVersion;
    std::string kind;
    std::vector<Device> devices;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Spec, cdiVersion, kind, devices);
};

} // namespace wsl::shared::cdi
