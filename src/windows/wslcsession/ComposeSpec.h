/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeSpec.h

Abstract:

    Defines the parsed representation of a compose file.

--*/

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::service::wslc {

struct ComposeContainerDefinition
{
    std::string ServiceName;
    std::string Name;
    std::string Image;
    std::vector<std::string> Command;
    std::vector<std::string> Environment;
    std::string WorkingDirectory;

    struct Volume
    {
        std::optional<std::wstring> HostPath;
        std::string Name;
        std::string ContainerPath;
        bool ReadOnly{};
    };

    struct Port
    {
        uint16_t HostPort{};
        uint16_t ContainerPort{};
    };

    std::vector<Volume> Volumes;
    std::vector<Port> Ports;
};

struct ComposeSpec
{
    std::vector<ComposeContainerDefinition> Containers;
    std::string ProjectName;

    static ComposeSpec Parse(const std::filesystem::path& Path, std::string_view Content);
};

} // namespace wsl::windows::service::wslc
