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
    std::vector<std::string> Warnings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreatedContainer, Id, Warnings);
};

struct ErrorResponse
{
    std::string message;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ErrorResponse, message);
};

struct EmptyRequest
{
    using TResponse = void;
};

struct EmptyObject
{
};

inline void to_json(nlohmann::json& j, const EmptyObject& memory)
{
    j = nlohmann::json::object();
}

struct Mount
{
    std::string Source;
    std::string Target;
    std::string Type;
    bool ReadOnly{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Mount, Target, Source, Type, ReadOnly);
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
    std::vector<std::string> Dns;
    std::vector<std::string> DnsSearch;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HostConfig, Mounts, PortBindings, NetworkMode, Init, Dns, DnsSearch);
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
    std::vector<std::string> Cmd;
    std::vector<std::string> Entrypoint; // TODO: Find a way to omit if the caller wants the default entrypoint.
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

struct Image
{
    std::string Id;
    std::vector<std::string> RepoTags;
    uint64_t Size{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Image, Id, RepoTags, Size);
};

struct DeletedImage
{
    std::string Untagged;
    std::string Deleted;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DeletedImage, Untagged, Deleted);
};

struct ImportStatus
{
    std::string status;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ImportStatus, status);
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateExec, AttachStdin, AttachStdout, AttachStderr, Tty, ConsoleSize, Cmd, Env, WorkingDir, User);
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
    ContainerState State{ContainerState::Unknown};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ContainerInfo, Id, Names, Image, Labels, Ports, State);
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

    CreateImageProgressDetails progressDetail;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CreateImageProgress, status, id, progressDetail);
};

struct BuildProgress
{
    std::string stream;
    std::string error;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BuildProgress, stream, error);
};

} // namespace wsl::windows::common::docker_schema