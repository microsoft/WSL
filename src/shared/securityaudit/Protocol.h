// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace wsl::audit {

constexpr uint32_t c_protocolVersion = 2;
constexpr size_t c_maxMessageSize = 64 * 1024;

inline uint32_t AuditSchemaVersion(const nlohmann::json& message, uint32_t ordinaryVersion)
{
    return ordinaryVersion + (message.contains("wslcSessionId") ? 1U : 0U);
}

inline constexpr const char* c_processBooleanFields[] = {
    "pidNamespaceMissing",
    "executableMissing",
    "cwdMissing",
    "executableTruncated",
    "cwdTruncated",
    "credentialsMissing",
    "uidMissing",
    "euidMissing",
    "gidMissing",
    "egidMissing",
    "startTimeMissing",
    "timestampMissing",
    "cmdlineMissing",
    "cmdlineTruncated"};

struct AuditPolicy
{
    bool processCreation{};
    bool includeCommandLine{};
    std::string source{"windows"};
    bool networkSuccess{};
    bool networkFailure{};
    bool moduleLoad{};
    bool privilegeTransition{};

    bool AnyEnabled() const
    {
        return processCreation || networkSuccess || networkFailure || moduleLoad || privilegeTransition;
    }
};

inline nlohmann::json PolicySettings(const AuditPolicy& policy)
{
    return {
        {"processCreation", policy.processCreation},
        {"includeCommandLine", policy.processCreation && policy.includeCommandLine},
        {"networkSuccess", policy.networkSuccess},
        {"networkFailure", policy.networkFailure},
        {"moduleLoad", policy.moduleLoad},
        {"privilegeTransition", policy.privilegeTransition}};
}

inline nlohmann::json ParseMessage(std::string_view text)
{
    if (text.empty() || text.size() > c_maxMessageSize)
    {
        throw std::invalid_argument("Invalid audit message size");
    }

    auto message = nlohmann::json::parse(text);
    if (!message.is_object() || !message.contains("version") || !message["version"].is_number_unsigned() ||
        message["version"].get<uint64_t>() != c_protocolVersion || !message.contains("kind") || !message["kind"].is_string())
    {
        throw std::invalid_argument("Invalid audit protocol envelope");
    }

    return message;
}

inline std::string EncodeMessage(const nlohmann::json& message)
{
    auto text = message.dump();
    if (text.size() > c_maxMessageSize)
    {
        throw std::invalid_argument("Audit message exceeds the protocol limit");
    }

    text.push_back('\n');
    return text;
}

inline nlohmann::json Configuration(const AuditPolicy& policy)
{
    auto message = PolicySettings(policy);
    message["version"] = c_protocolVersion;
    message["kind"] = "configure";
    message["policySource"] = policy.source;
    return message;
}

inline AuditPolicy ParseConfiguration(std::string_view text)
{
    const auto message = ParseMessage(text);
    if (message["kind"] != "configure" || message.size() != 9 || !message.contains("policySource") || !message["policySource"].is_string())
    {
        throw std::invalid_argument("Invalid audit configuration");
    }

    const auto source = message["policySource"].get<std::string>();
    if (source != "windows")
    {
        throw std::invalid_argument("Unknown audit policy source");
    }

    for (const auto* name :
         {"processCreation", "includeCommandLine", "networkSuccess", "networkFailure", "moduleLoad", "privilegeTransition"})
    {
        if (!message.contains(name) || !message[name].is_boolean())
        {
            throw std::invalid_argument(std::string("Invalid audit policy flag: ") + name);
        }
    }
    AuditPolicy policy{
        message["processCreation"].get<bool>(),
        message["includeCommandLine"].get<bool>(),
        source,
        message["networkSuccess"].get<bool>(),
        message["networkFailure"].get<bool>(),
        message["moduleLoad"].get<bool>(),
        message["privilegeTransition"].get<bool>()};
    if (policy.includeCommandLine && !policy.processCreation)
    {
        throw std::invalid_argument("Command-line collection requires process success auditing");
    }
    return policy;
}

inline void ValidateProcessEvent(const nlohmann::json& message)
{
    if (!message.is_object() || !message.contains("version") || !message["version"].is_number_unsigned() ||
        message["version"].get<uint64_t>() != c_protocolVersion || !message.contains("kind") || message["kind"] != "process")
    {
        throw std::invalid_argument("Invalid process event envelope");
    }

    for (const auto* name : {"pid", "tid", "ppid", "pidNamespace", "uid", "euid", "gid", "egid"})
    {
        const auto& value = message.at(name);
        if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
        {
            throw std::invalid_argument(std::string("Invalid process event field: ") + name);
        }
    }

    for (const auto* name : {"timestampNs", "startTimeNs"})
    {
        if (!message.at(name).is_number_unsigned())
        {
            throw std::invalid_argument(std::string("Invalid process event field: ") + name);
        }
    }

    for (const auto* name : {"namespacePid", "mountNamespace", "networkNamespace", "cgroupId"})
    {
        const auto value = message.find(name);
        if (value != message.end() &&
            (!value->is_number_unsigned() || (std::string_view(name) != "cgroupId" && value->get<uint64_t>() > UINT32_MAX)))
        {
            throw std::invalid_argument(std::string("Invalid optional process context: ") + name);
        }
    }

    for (const auto* name : {"executable", "cwd"})
    {
        if (!message.at(name).is_string() || message.at(name).get_ref<const std::string&>().size() > 4096)
        {
            throw std::invalid_argument(std::string("Invalid process event path: ") + name);
        }
    }

    if (!message.at("cmdlineTruncated").is_boolean())
    {
        throw std::invalid_argument("Invalid command-line truncation flag");
    }

    for (const auto* name : c_processBooleanFields)
    {
        if (message.contains(name) && !message[name].is_boolean())
        {
            throw std::invalid_argument(std::string("Invalid process availability flag: ") + name);
        }
    }

    if (message.contains("argv"))
    {
        const auto& arguments = message.at("argv");
        if (!arguments.is_array())
        {
            throw std::invalid_argument("Invalid process arguments");
        }

        size_t total = 0;
        for (const auto& argument : arguments)
        {
            if (!argument.is_string())
            {
                throw std::invalid_argument("Invalid process argument");
            }

            total += argument.get_ref<const std::string&>().size() + 1;
            if (total > 4096)
            {
                throw std::invalid_argument("Process arguments exceed the collection limit");
            }
        }
    }
}

} // namespace wsl::audit
