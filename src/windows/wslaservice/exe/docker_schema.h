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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(CreateContainer, Image, Cmd, Tty, OpenStdin, StdinOnce, Entrypoint, Env, ExposedPorts);
};

} // namespace wsl::windows::service::wsla::docker_schema