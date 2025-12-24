#pragma once

#include "JsonUtils.h"

namespace wsl::windows::service::wsla::docker_schema {

struct CreatedContainer
{
    std::string Id;
    std::vector<std::string> Warnings;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreatedContainer, Id, Warnings);
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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(CreateContainer, Image, Cmd, Tty, OpenStdin, StdinOnce);
};

} // namespace wsl::windows::service::wsla::docker_schema