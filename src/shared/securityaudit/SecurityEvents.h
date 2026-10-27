// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Protocol.h"
#include <array>
#include <span>

namespace wsl::audit {

inline bool AllowsSecurityEvent(const AuditPolicy& policy, std::string_view family, std::string_view outcome)
{
    if (outcome != "success" && outcome != "failure" && outcome != "pending" && outcome != "unknown")
    {
        throw std::invalid_argument("Unknown security event outcome");
    }
    if (family == "network")
    {
        if (outcome == "success")
        {
            return policy.networkSuccess;
        }
        if (outcome == "failure")
        {
            return policy.networkFailure;
        }
        return policy.networkSuccess && policy.networkFailure;
    }
    if (family == "process-creation")
    {
        return policy.processCreation && outcome == "success";
    }
    if (family == "module-load")
    {
        return policy.moduleLoad && outcome == "success";
    }
    if (family == "privilege-transition")
    {
        return policy.privilegeTransition && outcome == "success";
    }
    if (family == "namespace-change" || family == "time-change")
    {
        return false;
    }
    throw std::invalid_argument("Unknown security policy family");
}

enum class SecurityFieldType
{
    Number,
    Text,
    Addresses,
    Flag,
    Unavailable
};

struct SecurityField
{
    const char* name;
    SecurityFieldType type;
};

inline std::span<const SecurityField> SecurityFields(std::string_view family)
{
    using T = SecurityFieldType;
    static constexpr SecurityField creation[] = {
        {"parentPid", T::Number},
        {"parentTid", T::Unavailable},
        {"childPid", T::Number},
        {"childTid", T::Number},
        {"flags", T::Unavailable},
        {"parentStartTimeNs", T::Number},
        {"namespacePid", T::Number},
        {"executable", T::Text},
        {"cwd", T::Text},
        {"executableTruncated", T::Flag},
        {"cwdTruncated", T::Flag}};
    static constexpr SecurityField network[] = {
        {"socketFd", T::Number},
        {"acceptedFd", T::Number},
        {"socketInode", T::Unavailable},
        {"socketFamily", T::Unavailable},
        {"socketType", T::Unavailable},
        {"requestedProtocol", T::Number},
        {"protocol", T::Unavailable},
        {"socketNetworkNamespace", T::Unavailable},
        {"localIp", T::Text},
        {"localPort", T::Number},
        {"remoteIp", T::Text},
        {"remotePort", T::Number},
        {"requestedIp", T::Text},
        {"requestedPort", T::Number},
        {"dnsName", T::Text},
        {"dnsAddresses", T::Addresses},
        {"dnsStatus", T::Number}};
    static constexpr SecurityField module[] = {
        {"moduleFd", T::Number},
        {"imageSize", T::Number},
        {"flags", T::Number},
        {"moduleName", T::Unavailable},
        {"modulePath", T::Unavailable},
        {"parameters", T::Unavailable},
        {"signer", T::Unavailable},
        {"hash", T::Unavailable}};
    static constexpr SecurityField privilege[] = {
        {"requestedUid", T::Number},
        {"requestedGid", T::Number},
        {"requestedRealGid", T::Number},
        {"requestedEffectiveGid", T::Number},
        {"requestedSavedGid", T::Number},
        {"oldUid", T::Number},
        {"oldGid", T::Number},
        {"newUid", T::Number},
        {"newGid", T::Number},
        {"capabilitiesBefore", T::Unavailable},
        {"capabilitiesAfter", T::Unavailable}};
    static constexpr SecurityField names[] = {
        {"requestedFlags", T::Number},
        {"namespaceFd", T::Number},
        {"namespaceTypes", T::Number},
        {"previousMountNamespace", T::Number},
        {"operationMountNamespace", T::Number},
        {"operationNetworkNamespace", T::Number},
        {"operationPidNamespace", T::Number},
        {"source", T::Unavailable},
        {"target", T::Unavailable},
        {"filesystemType", T::Unavailable}};
    static constexpr SecurityField time[] = {
        {"oldTime", T::Unavailable}, {"newTime", T::Unavailable}, {"offset", T::Unavailable}, {"privilege", T::Unavailable}};
    if (family == "process-creation")
    {
        return creation;
    }
    if (family == "network")
    {
        return network;
    }
    if (family == "module-load")
    {
        return module;
    }
    if (family == "privilege-transition")
    {
        return privilege;
    }
    if (family == "namespace-change")
    {
        return names;
    }
    if (family == "time-change")
    {
        return time;
    }
    throw std::invalid_argument("Unknown security event family");
}

inline std::string_view SecurityFamily(std::string_view operation)
{
    if (operation == "process-created")
    {
        return "process-creation";
    }
    if (operation == "socket" || operation == "bind" || operation == "connect" || operation == "accept" ||
        operation == "accept4" || operation == "dns-query")
    {
        return "network";
    }
    if (operation == "init_module" || operation == "finit_module")
    {
        return "module-load";
    }
    if (operation == "setuid" || operation == "setgid" || operation == "setresgid")
    {
        return "privilege-transition";
    }
    if (operation == "unshare" || operation == "setns")
    {
        return "namespace-change";
    }
    // No time-change operation is emitted by the validated library.
    throw std::invalid_argument("Unsupported security operation");
}

inline nlohmann::json LibraryCoverage()
{
    return {
        {"sourceRevision", "e964b0e378330f9b5b5a2fd74a2c728d938e9167"},
        {"complete", false},
        {"process-execution",
         {{"supported", {"successful-execution"}},
          {"unsupported", {"failed-execution", "execve-vs-execveat-attribution", "capabilities"}}}},
        {"process-creation",
         {{"supported", {"successful-process-creation"}},
          {"unsupported", {"creation-api-attribution", "parent-tid", "clone-flags", "thread-creation", "failed-creation"}}}},
        {"network",
         {{"supported", {"socket", "bind", "connect", "accept", "accept4", "dns-query"}},
          {"unsupported",
           {"listen",
            "sendmsg",
            "async-connect-completion",
            "socket-inode",
            "socket-family",
            "socket-type",
            "actual-protocol",
            "socket-network-namespace",
            "bind-assigned-port",
            "dns-to-connection-correlation"}}}},
        {"module-load",
         {{"supported", {"init_module", "finit_module"}},
          {"unsupported", {"module-name", "module-path", "parameters", "signer", "hash"}},
          {"unverified", {"successful-module-load"}}}},
        {"privilege-transition",
         {{"supported", {"setuid", "setgid", "setresgid"}},
          {"unsupported", {"setresuid", "capset", "changed-identity-before-snapshot", "capabilities"}}}},
        {"namespace-change",
         {{"supported", {"unshare", "setns"}},
          {"unsupported", {"mount", "umount2", "pivot_root", "non-mount-and-non-network-namespace-changes"}}}},
        {"time-change",
         {{"supported", nlohmann::json::array()},
          {"unsupported", {"clock_settime", "clock_adjtime", "settimeofday", "adjtimex"}}}}};
}

inline nlohmann::json EmptySecurityFields(std::string_view family)
{
    auto result = nlohmann::json::object();
    for (const auto& field : SecurityFields(family))
    {
        result[field.name] = nullptr;
    }
    return result;
}

inline void ValidateSecurityEvent(const nlohmann::json& event)
{
    if (!event.is_object() || event.at("version") != c_protocolVersion || !event.at("version").is_number_unsigned() ||
        event.at("kind") != "security" || !event.at("operation").is_string() || !event.at("family").is_string() ||
        SecurityFamily(event.at("operation").get_ref<const std::string&>()) != event.at("family").get_ref<const std::string&>())
    {
        throw std::invalid_argument("Invalid security event envelope");
    }
    for (const auto* name : {"pid", "tid", "pidNamespace"})
    {
        const auto& value = event.at(name);
        if (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX)
        {
            throw std::invalid_argument(std::string("Invalid security identity: ") + name);
        }
    }
    for (const auto* name : {"uid", "euid", "gid", "egid", "mountNamespace", "networkNamespace", "collectionStatus"})
    {
        const auto& value = event.at(name);
        if (!value.is_null() && (!value.is_number_unsigned() || value.get<uint64_t>() > UINT32_MAX))
        {
            throw std::invalid_argument(std::string("Invalid optional security identity: ") + name);
        }
    }
    for (const auto* name : {"timestampNs", "startTimeNs", "cgroupId"})
    {
        if (!event.at(name).is_null() && !event.at(name).is_number_unsigned())
        {
            throw std::invalid_argument(std::string("Invalid security clock/context: ") + name);
        }
    }
    const auto& result = event.at("result");
    if (!result.is_null() && (!result.is_number_integer() || (result.is_number_unsigned() && result.get<uint64_t>() > INT64_MAX)))
    {
        throw std::invalid_argument("Invalid security return value");
    }
    const auto outcome = event.at("operation") == "process-created"
                             ? "success"
                             : (event.at("operation") == "dns-query"
                                    ? (event.at("fields").at("dnsStatus") == 0 ? "success" : "failure")
                                    : (result.is_null() ? "unknown"
                                                        : (event.at("operation") == "connect" && result.get<int64_t>() == -115
                                                               ? "pending"
                                                               : (result.get<int64_t>() < 0 ? "failure" : "success"))));
    if ((event.at("operation") == "process-created" && !result.is_null()) ||
        (event.at("operation") == "dns-query" && event.at("fields").at("dnsStatus") != 0 && event.at("fields").at("dnsStatus") != 1))
    {
        throw std::invalid_argument("Invalid synthetic event result");
    }
    if (event.at("outcome") != outcome)
    {
        throw std::invalid_argument("Security outcome disagrees with return value");
    }
    const auto fields = SecurityFields(event.at("family").get_ref<const std::string&>());
    if (!event.at("fields").is_object() || event.at("fields").size() != fields.size())
    {
        throw std::invalid_argument("Invalid security field set");
    }
    for (const auto& field : fields)
    {
        const auto& value = event.at("fields").at(field.name);
        if (value.is_null())
        {
            continue;
        }
        bool valid = false;
        switch (field.type)
        {
        case SecurityFieldType::Number:
        {
            const std::string_view name(field.name);
            if (name.ends_with("Fd") || name == "requestedProtocol")
            {
                valid = value.is_number_integer() &&
                        (value.is_number_unsigned() ? value.get<uint64_t>() <= INT32_MAX
                                                    : value.get<int64_t>() >= INT32_MIN && value.get<int64_t>() <= INT32_MAX);
            }
            else
            {
                const uint64_t maximum = name.ends_with("Port")
                                             ? UINT16_MAX
                                             : (name == "flags" || name == "requestedFlags" || name == "imageSize" ||
                                                        name == "parentStartTimeNs" || name == "socketInode"
                                                    ? UINT64_MAX
                                                    : UINT32_MAX);
                valid = value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0) &&
                        value.get<uint64_t>() <= maximum;
            }
            break;
        }
        case SecurityFieldType::Text:
            valid = value.is_string() && value.get_ref<const std::string&>().size() <= 4096;
            break;
        case SecurityFieldType::Addresses:
            valid = value.is_array() && value.size() <= 16;
            if (valid)
            {
                for (const auto& address : value)
                {
                    valid = valid && address.is_string() && address.get_ref<const std::string&>().size() <= 64;
                }
            }
            break;
        case SecurityFieldType::Unavailable:
            break;
        case SecurityFieldType::Flag:
            valid = value.is_boolean();
            break;
        }
        if (!valid)
        {
            throw std::invalid_argument(std::string("Invalid security field: ") + field.name);
        }
    }
    if (event.at("family") == "network")
    {
        const auto& values = event.at("fields");
        const bool dns = event.at("operation") == "dns-query";
        for (const auto& field : fields)
        {
            const bool dnsField = std::string_view(field.name).starts_with("dns");
            if (dns != dnsField && !values.at(field.name).is_null())
            {
                throw std::invalid_argument("DNS and socket observations must remain separate");
            }
        }
    }
}

inline nlohmann::json SelectSecurityEvent(const nlohmann::json& event)
{
    ValidateSecurityEvent(event);
    auto result = nlohmann::json::object();
    for (const auto* name :
         {"version",
          "kind",
          "family",
          "operation",
          "pid",
          "tid",
          "pidNamespace",
          "uid",
          "euid",
          "gid",
          "egid",
          "mountNamespace",
          "networkNamespace",
          "cgroupId",
          "timestampNs",
          "startTimeNs",
          "collectionStatus",
          "result",
          "outcome",
          "fields"})
    {
        result[name] = event.at(name);
    }
    return result;
}

} // namespace wsl::audit
