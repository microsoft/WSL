/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    docker_schema.h

Abstract:

    JSON schema for the docker API.
    Targets the daemon API version bundled with WSLC's dockerd (currently v25.0.3, API v1.44).
    The documentation for the API can be found at: https://docs.docker.com/reference/api/engine/version/v1.44/#tag/Container

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::common::docker_schema {

using wsl::shared::EmptyObject;

struct CreatedContainer
{
    std::string Id;
    std::vector<std::string> Warnings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreatedContainer, Id, Warnings);
};

struct ErrorResponse
{
    std::string message;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ErrorResponse, message);
};

struct ImageLoadResult
{
    std::optional<std::string> stream;
    std::optional<std::string> status;
    std::optional<ErrorResponse> errorDetail;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageLoadResult, stream, status, errorDetail);
};

struct EmptyRequest
{
    using TResponse = void;
};

struct AuthRequest
{
    using TResponse = struct AuthResponse;

    std::string username;
    std::string password;
    std::string serveraddress;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AuthRequest, username, password, serveraddress);
};

struct AuthResponse
{
    std::string Status;
    std::optional<std::string> IdentityToken;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AuthResponse, Status, IdentityToken);
};

struct VolumeUsageData
{
    int64_t Size{-1};
    int64_t RefCount{-1};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VolumeUsageData, Size, RefCount);
};

struct Volume
{
    std::string Name;
    std::string Driver;
    std::string Mountpoint;
    std::string CreatedAt;
    std::optional<std::map<std::string, std::string>> Options;
    std::optional<std::map<std::string, std::string>> Labels;
    // Docker's wire schema for Status is map[string]any: third-party volume
    // drivers may set arbitrary JSON values (numbers, bools, objects), not
    // just strings. Use nlohmann::json so deserialization never throws.
    std::optional<std::map<std::string, nlohmann::json>> Status;
    std::optional<VolumeUsageData> UsageData;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Volume, Name, Driver, Mountpoint, CreatedAt, Options, Labels, Status, UsageData);
};

struct CreateVolume
{
    using TResponse = Volume;

    std::string Name;
    std::string Driver;
    std::map<std::string, std::string> DriverOpts;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateVolume, Name, Driver, DriverOpts, Labels);
};

struct ListVolumesResponse
{
    std::vector<Volume> Volumes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ListVolumesResponse, Volumes);
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

struct CreateNetworkResponse
{
    std::string Id;
    std::string Warning;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateNetworkResponse, Id, Warning);
};

struct CreateNetwork
{
    using TResponse = CreateNetworkResponse;

    std::string Name;
    std::string Driver;
    bool Internal{};
    std::optional<IPAM> IPAM;
    std::optional<std::map<std::string, std::string>> Options;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateNetwork, Name, Driver, Internal, IPAM, Options, Labels);
};

struct Network
{
    std::string Id;
    std::string Name;
    std::string Driver;
    std::string Scope;
    bool Internal{};
    IPAM IPAM;
    std::optional<std::map<std::string, std::string>> Options;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Network, Id, Name, Driver, Scope, Internal, IPAM, Options, Labels);
};

struct ContainerNetworkRequest
{
    using TResponse = void;
    std::string Container;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ContainerNetworkRequest, Container);
};

struct Mount
{
    std::string Name;
    std::string Source;
    std::string Target;
    std::string Type;
    bool ReadOnly{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Mount, Name, Target, Source, Type, ReadOnly);
};

struct DeviceMapping
{
    std::string PathOnHost;
    std::string PathInContainer;
    std::string CgroupPermissions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DeviceMapping, PathOnHost, PathInContainer, CgroupPermissions);
};

struct PortMapping
{
    std::string HostIp;
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PortMapping, HostIp, HostPort);
};

struct Ulimit
{
    std::string Name;
    std::int64_t Soft{};
    std::int64_t Hard{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Ulimit, Name, Soft, Hard);
};

struct DeviceRequest
{
    std::string Driver;
    std::vector<std::string> DeviceIDs;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DeviceRequest, Driver, DeviceIDs);
};

struct HostConfig
{
    std::vector<Mount> Mounts;
    std::map<std::string, std::vector<PortMapping>> PortBindings;
    std::string NetworkMode;
    bool Init{};
    std::optional<std::vector<std::string>> Dns;
    std::optional<std::vector<std::string>> DnsSearch;
    std::optional<std::vector<std::string>> DnsOptions;
    std::optional<std::vector<std::string>> Binds;
    std::map<std::string, std::string> Tmpfs;
    // Docker wire type is int64. 0 means "use daemon default" — same as omitting
    // the field — so we don't bother with std::optional here.
    std::int64_t ShmSize{};
    std::optional<std::vector<DeviceMapping>> Devices;
    std::optional<std::vector<DeviceRequest>> DeviceRequests;

    // Per-container resource limits. 0 means "no limit" (Docker default).
    std::int64_t Memory{};
    std::int64_t NanoCpus{};
    std::optional<std::vector<Ulimit>> Ulimits;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        HostConfig, Mounts, PortBindings, NetworkMode, Init, Dns, DnsSearch, DnsOptions, Binds, Tmpfs, Devices, DeviceRequests, ShmSize, Memory, NanoCpus, Ulimits);
};

struct EndpointSettings
{
    std::string IPAddress;
    std::string Gateway;
    std::string MacAddress;
    int IPPrefixLen{};
    std::optional<std::vector<std::string>> Aliases;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EndpointSettings, IPAddress, Gateway, MacAddress, IPPrefixLen, Aliases);
};

struct EndpointConfig
{
    std::optional<std::vector<std::string>> Aliases;
};

inline void to_json(nlohmann::json& j, const EndpointConfig& v)
{
    j = nlohmann::json::object();
    if (v.Aliases.has_value() && !v.Aliases->empty())
    {
        j["Aliases"] = *v.Aliases;
    }
}

struct NetworkingConfig
{
    std::map<std::string, EndpointConfig> EndpointsConfig;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(NetworkingConfig, EndpointsConfig);
};

struct NetworkSettings
{
    std::map<std::string, EndpointSettings> Networks;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkSettings, Networks);
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

struct CreateContainer
{
    using TResponse = CreatedContainer;

    std::string Image;
    bool Tty{};
    bool OpenStdin{};
    bool StdinOnce{};
    bool AttachStdin{};
    bool AttachStdout{};
    bool AttachStderr{};
    std::optional<std::string> User;
    std::string Hostname;
    std::string Domainname;
    std::optional<std::string> StopSignal;
    std::optional<long> StopTimeout;
    std::optional<std::string> WorkingDir;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::vector<std::string> Env;
    std::map<std::string, EmptyObject> ExposedPorts;
    std::map<std::string, std::string> Labels;
    std::optional<HealthConfig> Healthcheck;
    HostConfig HostConfig;
    NetworkingConfig NetworkingConfig;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(
        CreateContainer, Image, Cmd, Tty, OpenStdin, StdinOnce, Entrypoint, Env, ExposedPorts, HostConfig, StopSignal, StopTimeout, WorkingDir, User, Hostname, Domainname, Labels, Healthcheck, NetworkingConfig);
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

struct ContainerConfig
{
    std::string Image;
    std::string User;
    std::string WorkingDir;
    std::optional<std::vector<std::string>> Env;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::optional<std::string> StopSignal;
    std::optional<int> StopTimeout;
    std::optional<HealthConfig> Healthcheck;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerConfig, Image, User, WorkingDir, Env, Cmd, Entrypoint, StopSignal, StopTimeout, Healthcheck);
};

struct InspectMount
{
    std::string Type;
    std::string Source;
    std::string Destination;
    bool RW{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectMount, Type, Source, Destination, RW);
};

struct InspectContainer
{
    std::string Id;
    std::string Name;
    std::string Created;
    std::string Image;
    ContainerInspectState State;
    ContainerConfig Config;
    HostConfig HostConfig;
    NetworkSettings NetworkSettings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectContainer, Id, Name, Created, Image, State, Config, HostConfig, NetworkSettings);
};

struct InspectExec
{
    // N.B. Pid is a non-nullable int in moby's schema; it is 0 until runc forks the user process. ExitCode is a *int and
    // is null until the exec exits.
    int Pid{};
    std::optional<int> ExitCode{};
    bool Running{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectExec, Pid, ExitCode, Running);
};

struct PruneContainerResult
{
    std::optional<std::vector<std::string>> ContainersDeleted; // Null if no containers were deleted.
    uint64_t SpaceReclaimed{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PruneContainerResult, ContainersDeleted, SpaceReclaimed);
};

struct Image
{
    std::string Id;
    std::vector<std::string> RepoTags;
    std::vector<std::string> RepoDigests;
    int64_t Size{};
    int64_t Created{};
    std::string ParentId;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Image, Id, RepoTags, RepoDigests, Size, Created, ParentId);
};

struct DeletedImage
{
    std::string Untagged;
    std::string Deleted;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DeletedImage, Untagged, Deleted);
};

struct PruneImageResult
{
    std::optional<std::vector<DeletedImage>> ImagesDeleted;
    uint64_t SpaceReclaimed{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PruneImageResult, ImagesDeleted, SpaceReclaimed);
};

struct PruneVolumeResult
{
    std::optional<std::vector<std::string>> VolumesDeleted;
    uint64_t SpaceReclaimed{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PruneVolumeResult, VolumesDeleted, SpaceReclaimed);
};

struct PruneNetworkResult
{
    std::optional<std::vector<std::string>> NetworksDeleted;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PruneNetworkResult, NetworksDeleted);
};

struct ImportStatus
{
    std::string status;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImportStatus, status);
};

struct ImageConfig
{
    std::string User;
    std::optional<std::map<std::string, EmptyObject>> ExposedPorts;
    std::optional<std::vector<std::string>> Env;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::optional<std::map<std::string, EmptyObject>> Volumes;
    std::string WorkingDir;
    std::optional<std::map<std::string, std::string>> Labels;
    std::string StopSignal;
    std::optional<std::vector<std::string>> Shell;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageConfig, User, ExposedPorts, Env, Cmd, Entrypoint, Volumes, WorkingDir, Labels, StopSignal, Shell);
};

struct RootFS
{
    std::string Type;
    std::optional<std::vector<std::string>> Layers;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RootFS, Type, Layers);
};

struct GraphDriverData
{
    std::string Name;
    std::optional<std::map<std::string, std::string>> Data;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GraphDriverData, Name, Data);
};

struct InspectImage
{
    std::string Id;
    std::optional<std::vector<std::string>> RepoTags;
    std::optional<std::vector<std::string>> RepoDigests;
    std::string Parent;
    std::string Comment;
    std::string Created;
    std::optional<ImageConfig> Config;
    std::string Author;
    std::string Architecture;
    std::string Variant;
    std::string Os;
    std::string OsVersion;
    int64_t Size{};
    std::optional<GraphDriverData> GraphDriver;
    std::optional<RootFS> RootFS;
    std::optional<std::map<std::string, std::string>> Metadata;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        InspectImage, Id, RepoTags, RepoDigests, Parent, Comment, Created, Config, Author, Architecture, Variant, Os, OsVersion, Size, GraphDriver, RootFS, Metadata);
};

struct CreateExecResponse
{
    std::string Id;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateExecResponse, Id);
};

struct CreateExec
{
    using TResponse = CreateExecResponse;

    bool AttachStdin{};
    bool AttachStdout{};
    bool AttachStderr{};
    bool Tty{};
    // Docker wire type is *[2]uint64. Sending an empty array on a TTY exec yields
    // a 0x0 console; the field must be omitted entirely when the caller didn't set it.
    std::vector<ULONG> ConsoleSize;
    std::vector<std::string> Cmd;
    std::vector<std::string> Env;
    std::optional<std::string> User;
    std::string WorkingDir;
    std::optional<std::string> DetachKeys;
};

inline void to_json(nlohmann::json& j, const CreateExec& v)
{
    j = nlohmann::json{
        {"AttachStdin", v.AttachStdin},
        {"AttachStdout", v.AttachStdout},
        {"AttachStderr", v.AttachStderr},
        {"Tty", v.Tty},
        {"Cmd", v.Cmd},
        {"Env", v.Env},
        {"WorkingDir", v.WorkingDir},
        {"User", v.User},
        {"DetachKeys", v.DetachKeys},
    };

    if (!v.ConsoleSize.empty())
    {
        j["ConsoleSize"] = v.ConsoleSize;
    }
}

struct StartExec
{
    using TResponse = void;
    bool Tty{};
    bool Detach{};
    // See CreateExec::ConsoleSize.
    std::vector<ULONG> ConsoleSize;
};

inline void to_json(nlohmann::json& j, const StartExec& v)
{
    j = nlohmann::json{{"Tty", v.Tty}, {"Detach", v.Detach}};

    if (!v.ConsoleSize.empty())
    {
        j["ConsoleSize"] = v.ConsoleSize;
    }
}

enum class ContainerState
{
    Unknown,
    Created,
    Running,
    Paused,
    Restarting,
    Exited,
    Removing,
    Dead
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ContainerState,
    {
        // Unknown is first so unrecognized strings (or missing field) deserialize to Unknown,
        // not to whatever the next entry happens to be.
        {ContainerState::Unknown, nullptr},
        {ContainerState::Created, "created"},
        {ContainerState::Running, "running"},
        {ContainerState::Paused, "paused"},
        {ContainerState::Restarting, "restarting"},
        {ContainerState::Exited, "exited"},
        {ContainerState::Removing, "removing"},
        {ContainerState::Dead, "dead"},
    });

struct Port
{
    uint16_t PrivatePort{};
    uint16_t PublicPort{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Port, PrivatePort, PublicPort);
};

struct ContainerInfo
{
    std::string Id;
    std::vector<std::string> Names;
    std::string Image;
    std::map<std::string, std::string> Labels;
    std::vector<Port> Ports;
    std::vector<Mount> Mounts;
    ContainerState State{ContainerState::Unknown};
    int64_t Created{};
    HostConfig HostConfig;
    NetworkSettings NetworkSettings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerInfo, Id, Names, Image, Labels, Ports, Mounts, State, Created, HostConfig, NetworkSettings);
};

struct BuildKitVertex
{
    std::string digest;
    std::string name;
    std::string started;
    std::string error;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildKitVertex, digest, name, started, error);
};

struct BuildKitStatus
{
    std::string id;
    std::string vertex;
    int64_t current{};
    int64_t total{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildKitStatus, id, vertex, current, total);
};

struct BuildKitLog
{
    std::string vertex;
    std::string data; // base64-encoded output
    int stream{};     // 1 = stdout, 2 = stderr

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildKitLog, vertex, data, stream);
};

struct BuildKitSolveStatus
{
    std::vector<BuildKitVertex> vertexes;
    std::vector<BuildKitStatus> statuses;
    std::vector<BuildKitLog> logs;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildKitSolveStatus, vertexes, statuses, logs);
};

struct CreateImageProgressDetails
{
    uint64_t current{};
    uint64_t total{};
    std::string unit;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateImageProgressDetails, current, total, unit);
};

struct CreateImageProgress
{
    std::string status;
    std::string id;
    std::optional<ErrorResponse> errorDetail;

    CreateImageProgressDetails progressDetail;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateImageProgress, status, id, progressDetail, errorDetail);
};

// Container stats (GET /containers/{id}/stats?stream=false)
// See: https://docs.docker.com/reference/api/engine/version/v1.44/#tag/Container/operation/ContainerStats

struct ContainerStatsCpuUsage
{
    uint64_t total_usage{};
    std::optional<std::vector<uint64_t>> percpu_usage;
    uint64_t usage_in_kernelmode{};
    uint64_t usage_in_usermode{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsCpuUsage, total_usage, percpu_usage, usage_in_kernelmode, usage_in_usermode);
};

struct ContainerStatsCpuStats
{
    ContainerStatsCpuUsage cpu_usage;
    uint64_t system_cpu_usage{};
    uint32_t online_cpus{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsCpuStats, cpu_usage, system_cpu_usage, online_cpus);
};

struct ContainerStatsMemoryStats
{
    uint64_t usage{};
    uint64_t limit{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsMemoryStats, usage, limit);
};

struct ContainerStatsNetworkEntry
{
    uint64_t rx_bytes{};
    uint64_t rx_packets{};
    uint64_t tx_bytes{};
    uint64_t tx_packets{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsNetworkEntry, rx_bytes, rx_packets, tx_bytes, tx_packets);
};

struct ContainerStatsBlkioEntry
{
    std::string op;
    uint64_t value{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsBlkioEntry, op, value);
};

struct ContainerStatsBlkioStats
{
    std::optional<std::vector<ContainerStatsBlkioEntry>> io_service_bytes_recursive;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsBlkioStats, io_service_bytes_recursive);
};

struct ContainerStatsPidsStats
{
    uint64_t current{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStatsPidsStats, current);
};

struct ContainerStats
{
    std::string id;
    std::string name;
    ContainerStatsCpuStats cpu_stats;
    ContainerStatsCpuStats precpu_stats;
    ContainerStatsMemoryStats memory_stats;
    std::optional<std::map<std::string, ContainerStatsNetworkEntry>> networks;
    ContainerStatsBlkioStats blkio_stats;
    ContainerStatsPidsStats pids_stats;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerStats, id, name, cpu_stats, precpu_stats, memory_stats, networks, blkio_stats, pids_stats);
};

} // namespace wsl::windows::common::docker_schema
