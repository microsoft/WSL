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

    [[noreturn]] void ThrowInvalidComposeFile(std::wstring_view message)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, message);
    }

    std::vector<std::string> ParseComposeStringList(
        const std::filesystem::path& path, const std::string& serviceName, const YAML::Node& node, std::string_view property)
    {
        if (!node)
        {
            return {};
        }

        if (node.IsScalar())
        {
            return {node.as<std::string>()};
        }

        if (!node.IsSequence())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeStringOrListExpected(path.wstring(), std::string{property}, serviceName));
        }

        std::vector<std::string> result;
        result.reserve(node.size());
        for (const auto& value : node)
        {
            if (!value.IsScalar())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeStringListExpected(path.wstring(), std::string{property}, serviceName));
            }

            result.emplace_back(value.as<std::string>());
        }

        return result;
    }

    // Entries without a value take the variable's value when it is set, and otherwise stay bare so the variable is unset.
    std::string ResolveEnvironmentEntry(std::string entry, const ComposeVariables& variables)
    {
        if (entry.find('=') != std::string::npos)
        {
            return entry;
        }

        auto value = variables.Find(entry);
        if (!value.has_value())
        {
            return entry;
        }

        return std::format("{}={}", entry, *value);
    }

    std::vector<std::string> ParseComposeEnvironment(
        const std::filesystem::path& path, const std::string& serviceName, const YAML::Node& node, const ComposeVariables& variables)
    {
        if (!node)
        {
            return {};
        }

        if (node.IsSequence())
        {
            auto result = ParseComposeStringList(path, serviceName, node, "environment");
            for (auto& entry : result)
            {
                entry = ResolveEnvironmentEntry(std::move(entry), variables);
            }

            return result;
        }

        if (!node.IsMap())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeEnvironmentMapOrListExpected(path.wstring(), serviceName));
        }

        std::vector<std::string> result;
        result.reserve(node.size());
        for (const auto& entry : node)
        {
            if (!entry.first.IsScalar() || (!entry.second.IsScalar() && !entry.second.IsNull()))
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeEnvironmentScalarsExpected(path.wstring(), serviceName));
            }

            auto name = entry.first.as<std::string>();
            if (entry.second.IsNull())
            {
                result.emplace_back(ResolveEnvironmentEntry(std::move(name), variables));
                continue;
            }

            result.emplace_back(std::format("{}={}", name, entry.second.as<std::string>()));
        }

        return result;
    }

    ComposeContainerDefinition::Volume ParseComposeVolume(
        const std::filesystem::path& path, const std::filesystem::path& baseDirectory, const std::string& serviceName, const YAML::Node& node)
    {
        if (!node.IsScalar())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeVolumeShortSyntaxExpected(path.wstring(), serviceName));
        }

        auto value = node.as<std::string>();
        bool readOnly = false;
        if (value.ends_with(":ro") || value.ends_with(":rw"))
        {
            readOnly = value.ends_with(":ro");
            value.resize(value.size() - 3);
        }

        // The final colon separates the Linux destination; Windows source paths can contain a drive colon.
        const auto separator = value.rfind(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeVolumeFormatInvalid(path.wstring(), value, serviceName));
        }

        auto source = value.substr(0, separator);
        auto destination = value.substr(separator + 1);
        if (!destination.starts_with('/'))
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeVolumeDestinationInvalid(path.wstring(), destination, serviceName));
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

        const auto resolvedPath = sourcePath.is_absolute() ? sourcePath : std::filesystem::absolute(baseDirectory / sourcePath);
        return {
            .HostPath = resolvedPath.lexically_normal().wstring(),
            .ContainerPath = std::move(destination),
            .ReadOnly = readOnly,
        };
    }

    ComposeContainerDefinition::Port ParseComposePort(const std::filesystem::path& path, const std::string& serviceName, const YAML::Node& node)
    {
        if (!node.IsScalar())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposePortsShortSyntaxExpected(path.wstring(), serviceName));
        }

        const auto value = node.as<std::string>();
        const auto separator = value.find(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == value.size() || separator != value.rfind(':'))
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposePortFormatInvalid(path.wstring(), value, serviceName));
        }

        const auto parsePort = [&](std::string_view text, bool allowZero) {
            uint16_t port{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || (!allowZero && port == 0))
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposePortNumberInvalid(path.wstring(), value, serviceName));
            }

            return port;
        };

        return {
            .HostPort = parsePort(std::string_view{value}.substr(0, separator), true),
            .ContainerPort = parsePort(std::string_view{value}.substr(separator + 1), false),
        };
    }

    enum class ComposeMergeMode
    {
        Merge,
        Override,
        Reset,
    };

    ComposeMergeMode GetMergeMode(const YAML::Node& node)
    {
        const auto& tag = node.Tag();
        if (tag == "!reset")
        {
            return ComposeMergeMode::Reset;
        }

        if (tag == "!override")
        {
            return ComposeMergeMode::Override;
        }

        return ComposeMergeMode::Merge;
    }

    // A service accumulated across documents, with the document that first defined it for error reporting.
    struct ComposeServiceState
    {
        std::filesystem::path SourcePath;
        ComposeContainerDefinition Definition;
    };

    constexpr std::array<std::string_view, 8> c_supportedServiceProperties{
        "name", "container_name", "image", "environment", "working_dir", "command", "volumes", "ports"};

    std::string_view EnvironmentKey(std::string_view entry)
    {
        return entry.substr(0, entry.find('='));
    }

    std::pair<uint16_t, uint16_t> PortKey(const ComposeContainerDefinition::Port& port)
    {
        return {port.HostPort, port.ContainerPort};
    }

    std::vector<ComposeContainerDefinition::Volume> ParseComposeVolumes(
        const std::filesystem::path& path, const std::filesystem::path& baseDirectory, const std::string& serviceName, const YAML::Node& node)
    {
        if (!node.IsSequence())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeVolumesListExpected(path.wstring(), serviceName));
        }

        std::vector<ComposeContainerDefinition::Volume> result;
        result.reserve(node.size());
        for (const auto& volume : node)
        {
            result.emplace_back(ParseComposeVolume(path, baseDirectory, serviceName, volume));
        }

        return result;
    }

    std::vector<ComposeContainerDefinition::Port> ParseComposePorts(const std::filesystem::path& path, const std::string& serviceName, const YAML::Node& node)
    {
        if (!node.IsSequence())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposePortsListExpected(path.wstring(), serviceName));
        }

        std::vector<ComposeContainerDefinition::Port> result;
        result.reserve(node.size());
        for (const auto& port : node)
        {
            result.emplace_back(ParseComposePort(path, serviceName, port));
        }

        return result;
    }

    // Replaces the target value, or clears it when the node is tagged !reset.
    template <typename T, typename TParse>
    void ApplyValue(const YAML::Node& node, T& target, TParse&& parse)
    {
        if (!node)
        {
            return;
        }

        target = GetMergeMode(node) == ComposeMergeMode::Reset ? T{} : parse(node);
    }

    // Adds parsed entries to the target list, replacing any existing entry with the same key.
    // A !override tag clears the list first; a !reset tag only clears it.
    template <typename T, typename TParse, typename TKey>
    void ApplyList(const YAML::Node& node, std::vector<T>& target, TParse&& parse, TKey&& key)
    {
        if (!node)
        {
            return;
        }

        const auto mode = GetMergeMode(node);
        if (mode != ComposeMergeMode::Merge)
        {
            target.clear();
        }

        if (mode == ComposeMergeMode::Reset)
        {
            return;
        }

        for (auto& entry : parse(node))
        {
            const auto existing = std::ranges::find(target, std::invoke(key, entry), key);
            if (existing != target.end())
            {
                *existing = std::move(entry);
                continue;
            }

            target.emplace_back(std::move(entry));
        }
    }

    void ApplyComposeService(
        const std::filesystem::path& path,
        const std::filesystem::path& baseDirectory,
        const std::string& serviceName,
        const YAML::Node& settings,
        const ComposeVariables& variables,
        ComposeContainerDefinition& definition)
    {
        for (const auto& setting : settings)
        {
            const auto key = setting.first.as<std::string>();
            if (std::ranges::find(c_supportedServiceProperties, key) == c_supportedServiceProperties.end())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposePropertyUnsupported(path.wstring(), key));
            }
        }

        ApplyValue(settings["name"] ? settings["name"] : settings["container_name"], definition.Name, [&](const YAML::Node& node) {
            auto name = node.as<std::string>();
            if (name.empty())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeServiceNameEmpty(path.wstring(), serviceName));
            }

            return name;
        });

        ApplyValue(settings["image"], definition.Image, [&](const YAML::Node& node) {
            if (!node.IsScalar())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeServiceImageRequired(path.wstring(), serviceName));
            }

            auto image = node.as<std::string>();
            if (image.empty())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeServiceImageEmpty(path.wstring(), serviceName));
            }

            return image;
        });

        ApplyValue(settings["working_dir"], definition.WorkingDirectory, [&](const YAML::Node& node) {
            if (!node.IsScalar() || !node.Scalar().starts_with('/'))
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeWorkingDirectoryInvalid(path.wstring(), serviceName));
            }

            return node.as<std::string>();
        });

        ApplyValue(settings["command"], definition.Command, [&](const YAML::Node& node) {
            if (node.IsScalar())
            {
                // TODO: Implement proper parsing
                return shared::string::Split(node.as<std::string>(), ' ');
            }

            return ParseComposeStringList(path, serviceName, node, "command");
        });

        ApplyList(
            settings["environment"],
            definition.Environment,
            [&](const YAML::Node& node) { return ParseComposeEnvironment(path, serviceName, node, variables); },
            [](const std::string& entry) { return EnvironmentKey(entry); });

        ApplyList(
            settings["volumes"],
            definition.Volumes,
            [&](const YAML::Node& node) { return ParseComposeVolumes(path, baseDirectory, serviceName, node); },
            &ComposeContainerDefinition::Volume::ContainerPath);

        ApplyList(
            settings["ports"], definition.Ports, [&](const YAML::Node& node) { return ParseComposePorts(path, serviceName, node); }, PortKey);
    }

    // Replaces variable references in every scalar value; keys and tags are left as written.
    void InterpolateNode(YAML::Node node, const ComposeVariables& variables, const std::filesystem::path& path)
    {
        switch (node.Type())
        {
        case YAML::NodeType::Scalar:
            if (node.Scalar().find('$') != std::string::npos)
            {
                node = variables.Interpolate(node.Scalar(), path);
            }
            break;
        case YAML::NodeType::Sequence:
            for (auto child : node)
            {
                InterpolateNode(child, variables, path);
            }
            break;
        case YAML::NodeType::Map:
            for (auto entry : node)
            {
                InterpolateNode(entry.second, variables, path);
            }
            break;
        default:
            break;
        }
    }

    void ParseComposeDocument(const ComposeSpecDocument& document, const ComposeVariables& variables, std::vector<ComposeServiceState>& services)
    {
        const auto& path = document.SourcePath;
        const auto root = YAML::Load(document.Content);
        InterpolateNode(root, variables, path);
        if (!root.IsMap())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeMapExpected(path.wstring()));
        }

        for (const auto& property : root)
        {
            if (!property.first.IsScalar())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposePropertyNameInvalid(path.wstring()));
            }

            const auto key = property.first.as<std::string>();
            if (key == "version")
            {
                if (!property.second.IsScalar())
                {
                    ThrowInvalidComposeFile(Localization::MessageWslcComposeVersionScalarExpected(path.wstring()));
                }

                // The version field is informational metadata.
                continue;
            }

            if (key != "services")
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposePropertyUnsupported(path.wstring(), key));
            }
        }

        const auto servicesNode = root["services"];
        if (!servicesNode)
        {
            return;
        }

        if (!servicesNode.IsMap())
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeServicesMapExpected(path.wstring()));
        }

        for (const auto& service : servicesNode)
        {
            if (!service.first.IsScalar())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeServiceMapExpected(path.wstring()));
            }

            const auto serviceName = service.first.as<std::string>();
            const auto mode = GetMergeMode(service.second);
            auto existing = std::ranges::find_if(
                services, [&](const ComposeServiceState& state) { return state.Definition.ServiceName == serviceName; });

            if (mode != ComposeMergeMode::Merge && existing != services.end())
            {
                services.erase(existing);
                existing = services.end();
            }

            if (mode == ComposeMergeMode::Reset)
            {
                continue;
            }

            if (!service.second.IsMap())
            {
                ThrowInvalidComposeFile(Localization::MessageWslcComposeServiceMapExpected(path.wstring()));
            }

            if (existing == services.end())
            {
                existing = services.insert(services.end(), ComposeServiceState{.SourcePath = path, .Definition = {.ServiceName = serviceName}});
            }

            ApplyComposeService(path, document.BaseDirectory, serviceName, service.second, variables, existing->Definition);
        }
    }

} // namespace

ComposeSpec ComposeSpec::Parse(const std::vector<ComposeSpecDocument>& documents, const ComposeVariables& variables)
{
    THROW_HR_IF(E_INVALIDARG, documents.empty());

    // Documents merge in order: later documents override scalars, merge environment and volumes by key, and append ports.
    std::vector<ComposeServiceState> services;
    for (const auto& document : documents)
    {
        try
        {
            ParseComposeDocument(document, variables, services);
        }
        catch (const YAML::Exception& exception)
        {
            ThrowInvalidComposeFile(Localization::MessageWslcComposeFileInvalid(
                document.SourcePath.wstring(), wsl::shared::string::MultiByteToWide(exception.what())));
        }
    }

    if (services.empty())
    {
        ThrowInvalidComposeFile(Localization::MessageWslcComposeServicesMapExpected(documents.front().SourcePath.wstring()));
    }

    ComposeSpec spec;
    spec.Containers.reserve(services.size());
    for (auto& service : services)
    {
        if (service.Definition.Image.empty())
        {
            ThrowInvalidComposeFile(
                Localization::MessageWslcComposeServiceImageRequired(service.SourcePath.wstring(), service.Definition.ServiceName));
        }

        spec.Containers.emplace_back(std::move(service.Definition));
    }

    return spec;
}

} // namespace wsl::windows::service::wslc
