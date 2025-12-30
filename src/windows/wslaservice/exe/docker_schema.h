#pragma once

#include "JsonUtils.h"

namespace wsl::windows::service::wsla::docker_schema {

struct CreatedContainer
{
    std::string Id;
    std::vector<std::string> Warnings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreatedContainer, Id, Warnings);
};

struct ErrorResponse
{
    std::string Message;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ErrorResponse, Message);
};

struct EmtpyRequest
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
    bool ReadOnly;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(Mount, Target, Source, Type, ReadOnly);
};

struct PortMapping
{
    std::string HostIp;
    std::string HostPort;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(PortMapping, HostIp, HostPort);
};

struct HostConfig
{
    std::vector<Mount> Mounts;
    std::map<std::string, std::vector<PortMapping>> PortBindings;
    std::string NetworkMode;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(HostConfig, Mounts, PortBindings, NetworkMode);
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
    std::vector<std::string> Cmd;
    std::vector<std::string> Entrypoint;
    std::vector<std::string> Env;
    std::map<std::string, EmptyObject> ExposedPorts;
    HostConfig HostConfig;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(CreateContainer, Image, Cmd, Tty, OpenStdin, StdinOnce, Entrypoint, Env, ExposedPorts, HostConfig);
};

} // namespace wsl::windows::service::wsla::docker_schema