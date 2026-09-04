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

using wsl::shared::EmptyObject;

struct InspectPortBinding
{
    std::string HostIp;
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectPortBinding, HostIp, HostPort);
};

struct InspectMount
{
    std::string Type;
    std::string Name;
    std::string Source;
    std::string Destination;
    bool ReadWrite{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectMount, Type, Name, Source, Destination, ReadWrite);
};

struct HealthcheckResult
{
    std::string Start;
    std::string End;
    int ExitCode{};
    std::string Output;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HealthcheckResult, Start, End, ExitCode, Output);
};

struct Health
{
    std::string Status;
    int FailingStreak{};
    std::vector<HealthcheckResult> Log;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Health, Status, FailingStreak, Log);
};

struct ContainerInspectState
{
    std::string Status;
    bool Running{};
    int ExitCode{};
    std::string StartedAt;
    std::string FinishedAt;
    std::optional<Health> Health;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerInspectState, Status, Running, ExitCode, StartedAt, FinishedAt, Health);
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

struct HealthConfig
{
    std::optional<std::vector<std::string>> Test;
    std::optional<std::int64_t> Interval;
    std::optional<std::int64_t> Timeout;
    std::optional<std::int64_t> StartPeriod;
    std::optional<int> Retries;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HealthConfig, Test, Interval, Timeout, StartPeriod, Retries);
};

struct ContainerConfig
{
    std::string Image;
    std::optional<std::vector<std::string>> Env;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::string User;
    std::string WorkingDir;
    std::optional<int> StopTimeout;
    std::optional<HealthConfig> Healthcheck;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerConfig, Image, Env, Cmd, Entrypoint, User, WorkingDir, StopTimeout, Healthcheck, Labels);
};

struct InspectEndpointIPAMConfig
{
    std::string IPv4Address;
    std::vector<std::string> LinkLocalIPs;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectEndpointIPAMConfig, IPv4Address, LinkLocalIPs);
};

struct InspectEndpointSettings
{
    std::string IPAddress;
    std::string Gateway;
    std::string MacAddress;
    int IPPrefixLen{};
    std::vector<std::string> Aliases;
    std::vector<std::string> Links;
    std::map<std::string, std::string> DriverOpts;
    std::optional<InspectEndpointIPAMConfig> IPAMConfig;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectEndpointSettings, IPAddress, Gateway, MacAddress, IPPrefixLen, Aliases, Links, DriverOpts, IPAMConfig);
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
    std::optional<std::int64_t> SizeRw;
    std::optional<std::int64_t> SizeRootFs;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        InspectContainer, Id, Name, Created, Image, State, HostConfig, Config, Ports, Mounts, Labels, NetworkSettings, SizeRw, SizeRootFs);
};

// Serializes a container inspect document. SizeRw and SizeRootFs are only populated when the
// daemon was asked to compute them, so the keys are omitted entirely when they have no value.
inline nlohmann::json ToInspectJson(const InspectContainer& container)
{
    nlohmann::json document = container;

    if (!container.SizeRw.has_value())
    {
        document.erase("SizeRw");
    }

    if (!container.SizeRootFs.has_value())
    {
        document.erase("SizeRootFs");
    }

    return document;
}

struct ImageConfig
{
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::optional<std::vector<std::string>> Env;
    std::optional<std::map<std::string, EmptyObject>> ExposedPorts;
    std::optional<std::map<std::string, std::string>> Labels;
    std::string StopSignal;
    std::string User;
    std::optional<std::map<std::string, EmptyObject>> Volumes;
    std::string WorkingDir;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageConfig, Cmd, Entrypoint, Env, ExposedPorts, Labels, StopSignal, User, Volumes, WorkingDir);
};

struct ImageRootFS
{
    std::string Type;
    std::optional<std::vector<std::string>> Layers;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageRootFS, Type, Layers);
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
    int64_t Size{};
    std::optional<std::map<std::string, std::string>> Metadata;
    std::optional<ImageConfig> Config;
    std::optional<ImageRootFS> RootFS;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        InspectImage, Id, RepoTags, RepoDigests, Parent, Comment, Created, Author, Architecture, Os, Size, Metadata, Config, RootFS);
};

struct InspectVolume
{
    std::string Name;
    std::string Driver;
    std::string CreatedAt;
    std::string Mountpoint;
    std::string Scope;
    std::optional<std::map<std::string, std::string>> Options;
    std::optional<std::map<std::string, std::string>> Labels;
    std::optional<std::map<std::string, std::string>> Status;
};

// Labels and options are reported as null rather than as empty objects, and the driver-specific
// status is omitted entirely when the driver reports nothing.
inline void to_json(nlohmann::json& j, const InspectVolume& volume)
{
    const auto mapOrNull = [](const std::optional<std::map<std::string, std::string>>& value) {
        return value.has_value() && !value->empty() ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };

    j = nlohmann::json::object();
    j["CreatedAt"] = volume.CreatedAt;
    j["Driver"] = volume.Driver;
    j["Labels"] = mapOrNull(volume.Labels);
    j["Mountpoint"] = volume.Mountpoint;
    j["Name"] = volume.Name;
    j["Options"] = mapOrNull(volume.Options);
    j["Scope"] = volume.Scope;

    if (volume.Status.has_value() && !volume.Status->empty())
    {
        j["Status"] = *volume.Status;
    }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(InspectVolume, Name, Driver, CreatedAt, Mountpoint, Scope, Options, Labels, Status);

struct IPAMConfig
{
    std::string Subnet;
    std::string Gateway;
    std::string IPRange;
};

// The gateway and ip range are omitted when unset rather than emitted as empty strings.
inline void to_json(nlohmann::json& j, const IPAMConfig& config)
{
    j = nlohmann::json::object();
    j["Subnet"] = config.Subnet;

    if (!config.Gateway.empty())
    {
        j["Gateway"] = config.Gateway;
    }

    if (!config.IPRange.empty())
    {
        j["IPRange"] = config.IPRange;
    }
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(IPAMConfig, Subnet, Gateway, IPRange);

struct IPAM
{
    std::string Driver;
    std::optional<std::vector<IPAMConfig>> Config;
    std::map<std::string, std::string> Options;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IPAM, Driver, Config, Options);
};

struct NetworkConfigFrom
{
    std::string Network;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkConfigFrom, Network);
};

struct NetworkContainer
{
    std::string Name;
    std::string EndpointID;
    std::string MacAddress;
    std::string IPv4Address;
    std::string IPv6Address;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkContainer, Name, EndpointID, MacAddress, IPv4Address, IPv6Address);
};

struct Network
{
    std::string Id;
    std::string Name;
    std::string Created;
    std::string Driver;
    std::string Scope;
    bool EnableIPv4{true};
    bool EnableIPv6{};
    bool Internal{};
    bool Attachable{};
    bool Ingress{};
    bool ConfigOnly{};
    NetworkConfigFrom ConfigFrom;
    IPAM IPAM;
    std::map<std::string, std::string> Options;
    std::map<std::string, std::string> Labels;
    std::map<std::string, NetworkContainer> Containers;
    nlohmann::json Status = nlohmann::json::object();

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Network, Id, Name, Created, Driver, Scope, EnableIPv4, EnableIPv6, Internal, Attachable, Ingress, ConfigOnly, ConfigFrom, IPAM, Options, Labels, Containers, Status);
};

// The network properties carried from the session to the CLI for "network list". Values keep their
// native types; the CLI renders the string output.
struct NetworkListEntry
{
    std::string Id;
    std::string Name;
    std::string Driver;
    std::string Scope;
    std::string Created;
    bool EnableIPv4{true};
    bool EnableIPv6{};
    bool Internal{};
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkListEntry, Id, Name, Driver, Scope, Created, EnableIPv4, EnableIPv6, Internal, Labels);
};

// The volume properties carried from the session to the CLI for "volume list". Values keep their
// native types; the CLI renders the string output.
struct VolumeListEntry
{
    std::string Name;
    std::string Driver;
    std::string Mountpoint;
    std::string Scope;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VolumeListEntry, Name, Driver, Mountpoint, Scope, Labels);
};

struct EventActor
{
    std::string ID;
    std::map<std::string, std::string> Attributes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EventActor, ID, Attributes);
};

struct Event
{
    std::string Type;
    std::string Action;
    EventActor Actor;
    std::int64_t time{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Event, Type, Action, Actor, time);
};

} // namespace wsl::windows::common::wslc_schema
