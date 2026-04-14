/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    docker_schema.h

Abstract:

    JSON schema for the docker API.
    The documentation for the API can be found at: https://docs.docker.com/reference/api/engine/version/v1.52/#tag/Container

--*/

#pragma once

#include "JsonUtils.h"

namespace wsl::windows::common::docker_schema {

struct CreatedContainer
{
    std::string Id;
    std::string Name;
    std::vector<std::string> Warnings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreatedContainer, Id, Warnings);
};

struct ErrorResponse
{
    std::string message;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ErrorResponse, message);
};

struct ImageLoadResult
{
    std::optional<std::string> stream;
    std::optional<ErrorResponse> errorDetail;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImageLoadResult, stream, errorDetail);
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

struct CreateVolume
{
    using TResponse = void;

    std::string Name;
    std::string Driver;
    std::map<std::string, std::string> DriverOpts;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateVolume, Name, Driver, DriverOpts, Labels);
};

struct Volume
{
    std::string Name;
    std::string Driver;
    std::string Mountpoint;
    std::optional<std::map<std::string, std::string>> Options;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Volume, Name, Driver, Mountpoint, Options, Labels);
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
    IPAM IPAM;
    std::map<std::string, std::string> Labels;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateNetwork, Name, Driver, Internal, IPAM, Labels);
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

struct EmptyObject
{
};

inline void to_json(nlohmann::json& j, const EmptyObject& memory)
{
    UNREFERENCED_PARAMETER(memory);
    j = nlohmann::json::object();
}

inline void from_json(const nlohmann::json& j, EmptyObject& obj)
{
    // EmptyObject has no fields, so nothing to deserialize
    UNREFERENCED_PARAMETER(j);
    UNREFERENCED_PARAMETER(obj);
}

struct Mount
{
    std::string Name;
    std::string Source;
    std::string Target;
    std::string Type;
    bool ReadOnly{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Mount, Name, Target, Source, Type, ReadOnly);
};

struct PortMapping
{
    std::string HostIp;
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(PortMapping, HostIp, HostPort);
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HostConfig, Mounts, PortBindings, NetworkMode, Init, Dns, DnsSearch, DnsOptions, Binds, Tmpfs);
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
    std::optional<std::string> WorkingDir;
    std::optional<std::vector<std::string>> Cmd;
    std::optional<std::vector<std::string>> Entrypoint;
    std::vector<std::string> Env;
    std::map<std::string, EmptyObject> ExposedPorts;
    std::map<std::string, std::string> Labels;
    HostConfig HostConfig;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(
        CreateContainer, Image, Cmd, Tty, OpenStdin, StdinOnce, Entrypoint, Env, ExposedPorts, HostConfig, StopSignal, WorkingDir, User, Hostname, Domainname, Labels);
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

struct ContainerConfig
{
    std::string Image;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerConfig, Image);
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectContainer, Id, Name, Created, Image, State, Config, HostConfig);
};

struct InspectExec
{
    std::optional<int> Pid{};
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
    uint64_t Size{};
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
    uint64_t Size{};
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
    std::vector<ULONG> ConsoleSize;
    std::vector<std::string> Cmd;
    std::vector<std::string> Env;
    std::optional<std::string> User;
    std::string WorkingDir;
    std::optional<std::string> DetachKeys;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateExec, AttachStdin, AttachStdout, AttachStderr, Tty, ConsoleSize, Cmd, Env, WorkingDir, User, DetachKeys);
};

struct StartExec
{
    using TResponse = void;
    bool Tty{};
    bool Detach{};
    std::vector<ULONG> ConsoleSize;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(StartExec, Tty, Detach, ConsoleSize);
};

enum class ContainerState
{
    Created,
    Running,
    Paused,
    Restarting,
    Exited,
    Removing,
    Dead,
    Unknown
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ContainerState,
    {
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerInfo, Id, Names, Image, Labels, Ports, Mounts, State, Created, HostConfig);
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildKitStatus, id, vertex);
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

} // namespace wsl::windows::common::docker_schema