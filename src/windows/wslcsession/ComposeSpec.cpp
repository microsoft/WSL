/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeSpec.cpp

Abstract:

    Parses compose YAML files.

--*/

#include "precomp.h"
#include "ComposeSpec.h"
#include <yaml-cpp/yaml.h>

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

    [[noreturn]] void ThrowInvalidComposeFile(const std::filesystem::path& Path, std::wstring_view Details)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcComposeFileInvalid(Path.wstring(), Details));
    }

    std::vector<std::string> ParseComposeStringList(
        const std::filesystem::path& Path, const std::string& ServiceName, const YAML::Node& Node, std::string_view Property)
    {
        if (!Node)
        {
            return {};
        }

        if (Node.IsScalar())
        {
            return {Node.as<std::string>()};
        }

        if (!Node.IsSequence())
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the '{}' property for service '{}' must be a string or list",
                    wsl::shared::string::MultiByteToWide(std::string(Property)),
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        std::vector<std::string> result;
        result.reserve(Node.size());
        for (const auto& value : Node)
        {
            if (!value.IsScalar())
            {
                ThrowInvalidComposeFile(
                    Path,
                    std::format(
                        L"the '{}' property for service '{}' must contain only strings",
                        wsl::shared::string::MultiByteToWide(std::string(Property)),
                        wsl::shared::string::MultiByteToWide(ServiceName)));
            }

            result.emplace_back(value.as<std::string>());
        }

        return result;
    }

    std::vector<std::string> ParseComposeEnvironment(const std::filesystem::path& Path, const std::string& ServiceName, const YAML::Node& Node)
    {
        if (!Node)
        {
            return {};
        }

        if (Node.IsSequence())
        {
            return ParseComposeStringList(Path, ServiceName, Node, "environment");
        }

        if (!Node.IsMap())
        {
            ThrowInvalidComposeFile(
                Path, std::format(L"the 'environment' property for service '{}' must be a map or list", wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        std::vector<std::string> result;
        result.reserve(Node.size());
        for (const auto& entry : Node)
        {
            if (!entry.first.IsScalar() || (!entry.second.IsScalar() && !entry.second.IsNull()))
            {
                ThrowInvalidComposeFile(
                    Path, std::format(L"the 'environment' property for service '{}' must contain scalar values", wsl::shared::string::MultiByteToWide(ServiceName)));
            }

            const auto name = entry.first.as<std::string>();
            const auto value = entry.second.IsNull() ? std::string{} : entry.second.as<std::string>();
            result.emplace_back(std::format("{}={}", name, value));
        }

        return result;
    }

    ComposeContainerDefinition::Volume ParseComposeVolume(const std::filesystem::path& Path, const std::string& ServiceName, const YAML::Node& Node)
    {
        if (!Node.IsScalar())
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the 'volumes' property for service '{}' must contain only short-syntax strings",
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        auto value = Node.as<std::string>();
        bool readOnly = false;
        if (value.ends_with(":ro") || value.ends_with(":rw"))
        {
            readOnly = value.ends_with(":ro");
            value.resize(value.size() - 3);
        }

        const auto separator = value.rfind(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the '{}' volume for service '{}' must use source:destination[:ro|rw] syntax",
                    wsl::shared::string::MultiByteToWide(value),
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        auto source = value.substr(0, separator);
        auto destination = value.substr(separator + 1);
        if (!destination.starts_with('/'))
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the '{}' volume destination for service '{}' must be an absolute Linux path",
                    wsl::shared::string::MultiByteToWide(destination),
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        const std::filesystem::path sourcePath = wsl::shared::string::MultiByteToWide(source);
        const bool bindMount = sourcePath.is_absolute() || source.starts_with('.') || source.find('/') != std::string::npos ||
                               source.find('\\') != std::string::npos;
        if (!bindMount)
        {
            return {
                .Name = std::move(source),
                .ContainerPath = std::move(destination),
                .ReadOnly = readOnly,
            };
        }

        const auto resolvedPath = sourcePath.is_absolute() ? sourcePath : std::filesystem::absolute(Path.parent_path() / sourcePath);
        return {
            .HostPath = resolvedPath.lexically_normal().wstring(),
            .ContainerPath = std::move(destination),
            .ReadOnly = readOnly,
        };
    }

    ComposeContainerDefinition::Port ParseComposePort(const std::filesystem::path& Path, const std::string& ServiceName, const YAML::Node& Node)
    {
        if (!Node.IsScalar())
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the 'ports' property for service '{}' must contain only host:container strings",
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        const auto value = Node.as<std::string>();
        const auto separator = value.find(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == value.size() || separator != value.rfind(':'))
        {
            ThrowInvalidComposeFile(
                Path,
                std::format(
                    L"the '{}' port for service '{}' must use host:container syntax",
                    wsl::shared::string::MultiByteToWide(value),
                    wsl::shared::string::MultiByteToWide(ServiceName)));
        }

        const auto parsePort = [&](std::string_view text, bool allowZero) {
            uint16_t port{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || (!allowZero && port == 0))
            {
                ThrowInvalidComposeFile(
                    Path,
                    std::format(
                        L"the '{}' port for service '{}' contains an invalid port number",
                        wsl::shared::string::MultiByteToWide(value),
                        wsl::shared::string::MultiByteToWide(ServiceName)));
            }

            return port;
        };

        return {
            .HostPort = parsePort(std::string_view{value}.substr(0, separator), true),
            .ContainerPort = parsePort(std::string_view{value}.substr(separator + 1), false),
        };
    }

    ComposeSpec ParseComposeFile(const std::filesystem::path& Path, std::string_view Content)
    {
        const auto root = YAML::Load(std::string{Content});
        if (!root.IsMap())
        {
            ThrowInvalidComposeFile(Path, L"the file must contain a map");
        }

        for (const auto& property : root)
        {
            if (!property.first.IsScalar())
            {
                ThrowInvalidComposeFile(Path, L"each top-level property name must be a string");
            }

            const auto key = property.first.as<std::string>();
            if (key == "version")
            {
                if (!property.second.IsScalar())
                {
                    ThrowInvalidComposeFile(Path, L"the top-level 'version' property must be a scalar value");
                }

                continue;
            }

            if (key != "services")
            {
                ThrowInvalidComposeFile(
                    Path, std::format(L"the top-level '{}' property is not supported", wsl::shared::string::MultiByteToWide(key)));
            }
        }

        const auto services = root["services"];
        if (!services || !services.IsMap() || services.size() == 0)
        {
            ThrowInvalidComposeFile(Path, L"the file must contain a non-empty services map");
        }

        ComposeSpec spec;

        spec.Containers.reserve(services.size());
        for (const auto& service : services)
        {
            if (!service.first.IsScalar() || !service.second.IsMap())
            {
                ThrowInvalidComposeFile(Path, L"each service must be a map");
            }

            const auto serviceName = service.first.as<std::string>();
            const auto& settings = service.second;
            for (const auto& setting : settings)
            {
                const auto key = setting.first.as<std::string>();
                if (key != "name" && key != "container_name" && key != "image" && key != "environment" && key != "working_dir" &&
                    key != "command" && key != "volumes" && key != "ports")
                {
                    ThrowInvalidComposeFile(
                        Path, std::format(L"the '{}' property is not supported", wsl::shared::string::MultiByteToWide(key)));
                }
            }

            const auto image = settings["image"];
            if (!image || !image.IsScalar())
            {
                ThrowInvalidComposeFile(
                    Path, std::format(L"the '{}' service must specify an image", wsl::shared::string::MultiByteToWide(serviceName)));
            }

            const auto nameNode = settings["name"] ? settings["name"] : settings["container_name"];
            const auto name = nameNode ? nameNode.as<std::string>() : std::string{};
            if (nameNode && name.empty())
            {
                ThrowInvalidComposeFile(
                    Path, std::format(L"the '{}' service has an empty name", wsl::shared::string::MultiByteToWide(serviceName)));
            }

            const auto imageName = image.as<std::string>();
            if (imageName.empty())
            {
                ThrowInvalidComposeFile(
                    Path, std::format(L"the '{}' service has an empty image", wsl::shared::string::MultiByteToWide(serviceName)));
            }

            ComposeContainerDefinition definition{
                .ServiceName = serviceName,
                .Name = name,
                .Image = imageName,
                .Environment = ParseComposeEnvironment(Path, serviceName, settings["environment"]),
            };

            const auto command = settings["command"];
            if (command && command.IsScalar())
            {
                // TODO: Implement proper parsing
                definition.Command = shared::string::Split(command.as<std::string>(), ' ');
            }
            else
            {
                definition.Command = ParseComposeStringList(Path, serviceName, command, "command");
            }

            const auto workingDirectory = settings["working_dir"];
            if (workingDirectory)
            {
                if (!workingDirectory.IsScalar())
                {
                    ThrowInvalidComposeFile(
                        Path, std::format(L"the 'working_dir' property for service '{}' must be a string", wsl::shared::string::MultiByteToWide(serviceName)));
                }

                definition.WorkingDirectory = workingDirectory.as<std::string>();
                if (!definition.WorkingDirectory.starts_with('/'))
                {
                    ThrowInvalidComposeFile(
                        Path,
                        std::format(
                            L"the working directory for service '{}' must be an absolute Linux path",
                            wsl::shared::string::MultiByteToWide(serviceName)));
                }
            }

            const auto volumes = settings["volumes"];
            if (volumes)
            {
                if (!volumes.IsSequence())
                {
                    ThrowInvalidComposeFile(
                        Path, std::format(L"the 'volumes' property for service '{}' must be a list", wsl::shared::string::MultiByteToWide(serviceName)));
                }

                definition.Volumes.reserve(volumes.size());
                for (const auto& volume : volumes)
                {
                    definition.Volumes.emplace_back(ParseComposeVolume(Path, serviceName, volume));
                }
            }

            const auto ports = settings["ports"];
            if (ports)
            {
                if (!ports.IsSequence())
                {
                    ThrowInvalidComposeFile(
                        Path, std::format(L"the 'ports' property for service '{}' must be a list", wsl::shared::string::MultiByteToWide(serviceName)));
                }

                definition.Ports.reserve(ports.size());
                for (const auto& port : ports)
                {
                    definition.Ports.emplace_back(ParseComposePort(Path, serviceName, port));
                }
            }

            spec.Containers.emplace_back(std::move(definition));
        }

        return spec;
    }

} // namespace

ComposeSpec ComposeSpec::Parse(const std::filesystem::path& Path, std::string_view Content)
{
    try
    {
        return ParseComposeFile(Path, Content);
    }
    catch (const YAML::Exception& exception)
    {
        ThrowInvalidComposeFile(Path, wsl::shared::string::MultiByteToWide(exception.what()));
    }
}

} // namespace wsl::windows::service::wslc
