/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsla_schema.h

Abstract:

    Contains the WSLA schema definitions for container operations.

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::common::wsla_schema {

struct InspectPortBinding
{
    std::string HostIp;
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(InspectPortBinding, HostIp, HostPort);
};

struct InspectMount
{
    std::string Type;
    std::string Source;
    std::string Destination;
    bool ReadWrite{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectMount, Type, Source, Destination, ReadWrite);
};

struct InspectState
{
    std::string Status;
    bool Running{};
    int ExitCode{};
    std::string StartedAt;
    std::string FinishedAt;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectState, Status, Running, ExitCode, StartedAt, FinishedAt);
};

struct InspectContainer
{
    std::string Id;
    std::string Name;
    std::string Created;
    std::string Image;

    InspectState State;

    std::string NetworkMode;
    std::map<std::string, std::vector<InspectPortBinding>> Ports;
    std::vector<InspectMount> Mounts;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectContainer, Id, Name, Created, Image, State, NetworkMode, Ports, Mounts);
};

} // namespace wsl::windows::common::wsla_schema