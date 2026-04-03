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

struct InspectState
{
    std::string Status;
    bool Running{};
    int ExitCode{};
    std::string StartedAt;
    std::string FinishedAt;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectState, Status, Running, ExitCode, StartedAt, FinishedAt);
};

struct InspectHostConfig
{
    std::string NetworkMode;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectHostConfig, NetworkMode);
};

struct InspectContainer
{
    std::string Id;
    std::string Name;
    std::string Created;
    std::string Image;
    InspectState State;
    InspectHostConfig HostConfig;
    std::map<std::string, std::vector<InspectPortBinding>> Ports;
    std::vector<InspectMount> Mounts;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectContainer, Id, Name, Created, Image, State, HostConfig, Ports, Mounts);
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

struct InspectVhdVolume
{
    std::string HostPath;
    uint64_t SizeBytes{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectVhdVolume, HostPath, SizeBytes);
};

struct InspectVolume
{
    std::string Name;
    std::string Type;
    std::optional<InspectVhdVolume> VhdVolume;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(InspectVolume, Name, Type, VhdVolume);
};

} // namespace wsl::windows::common::wslc_schema
