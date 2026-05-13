/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslc_schema.h

Abstract:

    Contains the WSLC schema definitions for container operations.

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::common::wslc_schema {

struct InspectPortBinding
{
    // WSLC always binds to localhost. Included for Docker API compatibility.
    std::string HostIp = "127.0.0.1";
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectPortBinding, HostIp, HostPort);
};

struct InspectMount
{
    // TODO: Support different mount types (plan9/VHD) when VHD volumes are implemented.
    std::string Type;
    std::string Source;
    std::string Destination;
    bool ReadWrite{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectMount, Type, Source, Destination, ReadWrite);
};

struct ContainerInspectState
{
    std::string Status;
    bool Running{};
    int ExitCode{};
    std::string StartedAt;
    std::string FinishedAt;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerInspectState, Status, Running, ExitCode, StartedAt, FinishedAt);
};

struct Ulimit
{
    std::string Name;
    std::int64_t Soft{};
    std::int64_t Hard{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Ulimit, Name, Soft, Hard);
};

struct InspectHostConfig
{
    std::string NetworkMode;
    std::int64_t Memory{};
    std::int64_t NanoCpus{};
    std::vector<Ulimit> Ulimits;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectHostConfig, NetworkMode, Memory, NanoCpus, Ulimits);
};

struct ContainerConfig
{
    std::optional<std::vector<std::string>> Env;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::string User;
    std::string WorkingDir;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerConfig, Env, Cmd, Entrypoint, User, WorkingDir);
};

struct InspectEndpointSettings
{
    std::string IPAddress;
    std::string Gateway;
    std::string MacAddress;
    int IPPrefixLen{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectEndpointSettings, IPAddress, Gateway, MacAddress, IPPrefixLen);
};

struct InspectNetworkSettings
{
    std::map<std::string, InspectEndpointSettings> Networks;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectNetworkSettings, Networks);
};

struct InspectContainer
{
    std::string Id;
    std::string Name;
    std::string Created;
    std::string Image;
    ContainerInspectState State;
    InspectHostConfig HostConfig;
    ContainerConfig Config;
    std::map<std::string, std::vector<InspectPortBinding>> Ports;
    std::vector<InspectMount> Mounts;
    std::map<std::string, std::string> Labels;
    InspectNetworkSettings NetworkSettings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectContainer, Id, Name, Created, Image, State, HostConfig, Config, Ports, Mounts, Labels, NetworkSettings);
};

struct ImageConfig
{
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::optional<std::vector<std::string>> Env;
    std::optional<std::map<std::string, std::string>> Labels;
    std::string User;
    std::string WorkingDir;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageConfig, Cmd, Entrypoint, Env, Labels, User, WorkingDir);
};

struct InspectImage
{
    std::string Id;
    std::optional<std::vector<std::string>> RepoTags;
    std::optional<std::vector<std::string>> RepoDigests;
    std::string Parent;
    std::string Comment;
    std::string Created;
    std::string Author;
    std::string Architecture;
    std::string Os;
    uint64_t Size{};
    std::optional<std::map<std::string, std::string>> Metadata;
    std::optional<ImageConfig> Config;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        InspectImage, Id, RepoTags, RepoDigests, Parent, Comment, Created, Author, Architecture, Os, Size, Metadata, Config);
};

struct InspectVolume
{
    std::string Name;
    std::string Driver;
    std::string CreatedAt;
    std::map<std::string, std::string> DriverOpts;
    std::map<std::string, std::string> Labels;
    std::optional<std::map<std::string, std::string>> Status;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectVolume, Name, Driver, CreatedAt, DriverOpts, Labels, Status);
};

struct IPAMConfig
{
    std::string Subnet;
    std::string Gateway;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IPAMConfig, Subnet, Gateway);
};

struct IPAM
{
    std::string Driver;
    std::optional<std::vector<IPAMConfig>> Config;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IPAM, Driver, Config);
};

struct Network
{
    std::string Id;
    std::string Name;
    std::string Driver;
    std::string Scope;
    bool Internal{};
    IPAM IPAM;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Network, Id, Name, Driver, Scope, Internal, IPAM, Labels);
};

} // namespace wsl::windows::common::wslc_schema
