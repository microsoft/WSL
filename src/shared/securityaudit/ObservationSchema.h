// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "SecurityEvents.h"

namespace wsl::audit {

inline constexpr uint32_t c_observationSchemaVersion = 3;
inline constexpr size_t c_observationFieldCount = 6;

enum class ObservationType
{
    Unsigned32,
    Signed32,
    Unsigned64,
    Signed64,
    Text,
    Addresses
};

struct ObservationField
{
    const char* name;
    const char* label;
    ObservationType type;
};

struct ObservationSchema
{
    uint32_t eventId;
    const char* operation;
    const char* eventType;
    const char* heading;
    bool deferred;
};

inline constexpr ObservationSchema c_observationSchemas[] = {
    {62020, "socket", "SocketCreated", "Socket creation", false},
    {62021, "bind", "SocketBound", "Requested socket binding", false},
    {62022, "connect", "SocketConnect", "Socket connection attempt", false},
    {62023, "accept", "SocketAccepted", "Socket accept", false},
    {62024, "accept4", "SocketAccepted4", "Socket accept4", false},
    {62025, "dns-query", "DnsQuery", "Resolver observation", false},
    {62030, "init_module", "ModuleLoaded", "Module image load", false},
    {62031, "finit_module", "ModuleLoadedFromFd", "Module load from file descriptor", false},
    {62040, "setuid", "UserIdChanged", "User identity request", false},
    {62041, "setgid", "GroupIdChanged", "Group identity request", false},
    {62042, "setresgid", "GroupIdsChanged", "Real, effective and saved group identity request", false},
    {62050, "unshare", "NamespaceUnshare", "Namespace unshare (policy deferred)", true},
    {62051, "setns", "NamespaceSet", "Namespace setns (policy deferred)", true}};

inline const ObservationSchema& GetObservationSchema(std::string_view operation)
{
    for (const auto& schema : c_observationSchemas)
    {
        if (schema.operation == operation)
        {
            return schema;
        }
    }
    throw std::invalid_argument("No readable observation schema for this operation");
}

inline constexpr ObservationField c_observationCommonFields[] = {
    {"result", "Reported return value (not a universal errno)", ObservationType::Signed64},
    {"vmPid", "Process ID (VM)", ObservationType::Unsigned32},
    {"vmTid", "Thread ID (VM)", ObservationType::Unsigned32},
    {"pidNamespace", "PID namespace ID", ObservationType::Unsigned32},
    {"mountNamespace", "Observed mount namespace ID", ObservationType::Unsigned32},
    {"networkNamespace", "Caller network namespace ID", ObservationType::Unsigned32},
    {"cgroupId", "Cgroup ID", ObservationType::Unsigned64},
    {"uid", "Observed user ID (UID)", ObservationType::Unsigned32},
    {"euid", "Observed effective user ID (EUID)", ObservationType::Unsigned32},
    {"gid", "Observed group ID (GID)", ObservationType::Unsigned32},
    {"egid", "Observed effective group ID (EGID)", ObservationType::Unsigned32},
    {"linuxEventNs", "Linux event clock (monotonic ns)", ObservationType::Unsigned64}};

inline std::span<const ObservationField> ObservationCommonFields(std::string_view operation)
{
    static constexpr ObservationField dns[] = {
        {"result", "Reported return value (not a universal errno)", ObservationType::Signed64},
        {"vmPid", "Process ID (VM)", ObservationType::Unsigned32},
        {"vmTid", "Thread ID (VM)", ObservationType::Unsigned32},
        {"pidNamespace", "PID namespace ID", ObservationType::Unsigned32},
        {"linuxEventNs", "Linux event clock (monotonic ns)", ObservationType::Unsigned64}};
    // PM C6 shared-context requests blocked by the DNS ABI: UID/EUID/GID/EGID,
    // mount/network namespace and cgroup. Add descriptors only with producer,
    // decoder, reader/version and test support; never emit unsupported slots.
    return operation == "dns-query" ? std::span<const ObservationField>(dns) : std::span(c_observationCommonFields);
}

inline std::span<const ObservationField> ObservationFields(std::string_view family, std::string_view operation)
{
    using T = ObservationType;
    static constexpr ObservationField socket[] = {{"requestedProtocol", "Requested protocol number", T::Signed32}};
    static constexpr ObservationField bind[] = {
        {"requestedIp", "Requested bind IP", T::Text}, {"requestedPort", "Requested bind port", T::Unsigned32}};
    static constexpr ObservationField connection[] = {
        {"localIp", "Observed local IP", T::Text},
        {"localPort", "Observed local port", T::Unsigned32},
        {"remoteIp", "Peer or requested destination IP", T::Text},
        {"remotePort", "Peer or requested destination port", T::Unsigned32}};
    static constexpr ObservationField dns[] = {{"dnsName", "Resolver query name", T::Text}};
    // PM C6 deferred: the ABI has no actual protocol or socket-owned network namespace;
    // separate DNS records cannot be attached to a connection merely by matching PID.
    // {"protocol", "Actual socket protocol", T::Unsigned32},
    // {"socketNetworkNamespace", "Socket network namespace ID", T::Unsigned32},
    // {"dnsName", "Connection DNS name", T::Text},
    // PM C17 deferred: the loader supplies pointers/FDs, not decoded identity/parameters.
    // Enable with a reliable producer/decoder, updated reader/version and tests.
    // {"moduleName", "Module name", T::Text},
    // {"modulePath", "Module path", T::Text},
    // {"parameters", "Module parameters", T::Text},
    // {"signer", "Module signer", T::Text},
    // {"hash", "Module hash", T::Text},
    static constexpr ObservationField setuid[] = {
        {"oldGid", "Previous group ID (unchanged dimension only)", T::Unsigned32},
        {"newUid", "Observed user ID after operation", T::Unsigned32},
        {"newGid", "Observed group ID after operation", T::Unsigned32}};
    static constexpr ObservationField setgid[] = {
        {"oldUid", "Previous user ID (unchanged dimension only)", T::Unsigned32},
        {"newUid", "Observed user ID after operation", T::Unsigned32},
        {"newGid", "Observed group ID after operation", T::Unsigned32}};
    // PM C19 deferred: no pre-change snapshot for the changed identity dimension.
    // {"oldUid", "Previous user ID", T::Unsigned32}, // setuid
    // {"oldGid", "Previous group ID", T::Unsigned32}, // setgid/setresgid
    // {"capabilitiesBefore", "Capabilities before operation", T::Text},
    // {"capabilitiesAfter", "Capabilities after operation", T::Text},
    // {"execCorrelation", "Execution correlation", T::Text}, // No verified correlation contract.
    static constexpr ObservationField names[] = {
        {"namespaceTypes", "Observed namespace type bits", T::Unsigned32},
        {"previousMountNamespace", "Previous mount namespace ID", T::Unsigned32},
        {"operationMountNamespace", "Operation mount namespace ID", T::Unsigned32},
        {"operationNetworkNamespace", "Operation network namespace ID", T::Unsigned32},
        {"operationPidNamespace", "Operation PID namespace ID", T::Unsigned32}};
    // PM C23 deferred until mount/umount/pivot_root producers exist and policy is approved.
    // {"source", "Mount source", T::Text},
    // {"target", "Mount target", T::Text},
    // {"filesystemType", "Filesystem type", T::Text},
    // PM C28: no selected time-change producer. Future schema needs API family,
    // old/new time or offset, caller/privilege, result and PID; do not fabricate events.
    if (family == "network")
    {
        if (operation == "socket")
        {
            return socket;
        }
        if (operation == "bind")
        {
            return bind;
        }
        return operation == "dns-query" ? std::span<const ObservationField>(dns) : std::span(connection);
    }
    if (family == "module-load")
    {
        return {};
    }
    if (family == "privilege-transition")
    {
        return operation == "setuid" ? std::span<const ObservationField>(setuid) : std::span(setgid);
    }
    if (family == "namespace-change")
    {
        return names;
    }
    throw std::invalid_argument("No readable fields for this security family");
}

inline std::string_view ObservationFieldState(const nlohmann::json& message, std::string_view name)
{
    const auto& values = message.at("fields");
    const auto operation = message.at("operation").get<std::string_view>();
    const auto family = SecurityFamily(operation);
    if (family == "network")
    {
        const bool dns = operation == "dns-query";
        if (dns != name.starts_with("dns"))
        {
            return "not_applicable";
        }
        if (!dns)
        {
            if (name == "protocol" || name == "socketNetworkNamespace")
            {
                return "unsupported";
            }
            if ((name == "requestedProtocol" && operation != "socket") ||
                (name == "acceptedFd" && (operation != "accept" && operation != "accept4")) ||
                ((name == "requestedIp" || name == "requestedPort") && operation != "bind") ||
                ((name == "remoteIp" || name == "remotePort") && (operation == "socket" || operation == "bind")) ||
                (name == "acceptedFd" && message.at("outcome") == "failure") ||
                (name == "socketFd" && operation == "socket" && message.at("outcome") == "failure"))
            {
                return "not_applicable";
            }
            if ((name.ends_with("Ip") || name.ends_with("Port")) && !values.at("socketFamily").is_null() &&
                values.at("socketFamily") != 2 && values.at("socketFamily") != 10)
            {
                return "not_applicable";
            }
        }
    }
    if (family == "module-load")
    {
        if (name == "moduleName" || name == "modulePath" || name == "parameters" || name == "signer" || name == "hash")
        {
            return "unsupported";
        }
        if ((operation == "init_module" && (name == "moduleFd" || name == "flags")) ||
            (operation == "finit_module" && name == "imageSize"))
        {
            return "not_applicable";
        }
    }
    if (family == "privilege-transition")
    {
        if (name.starts_with("capabilities") || (name == "oldUid" && operation == "setuid") || (name == "oldGid" && operation != "setuid"))
        {
            return "unsupported";
        }
        if ((name == "requestedUid" && operation != "setuid") || (name == "requestedGid" && operation != "setgid") ||
            (name.starts_with("requested") && name.ends_with("Gid") && name != "requestedGid" && operation != "setresgid"))
        {
            return "not_applicable";
        }
    }
    if (family == "namespace-change")
    {
        if (name == "source" || name == "target" || name == "filesystemType")
        {
            return "not_applicable";
        }
        if (name == "namespaceFd" && operation == "unshare")
        {
            return "not_applicable";
        }
    }
    return "available";
}

inline nlohmann::json SecurityObservationData(const nlohmann::json& message)
{
    using Json = nlohmann::json;
    ValidateSecurityEvent(message);
    const auto operation = message.at("operation").get<std::string_view>();
    const auto& schema = GetObservationSchema(operation);
    const auto family = SecurityFamily(operation);
    if (message.at("pid") == 0 || message.at("tid") == 0)
    {
        throw std::invalid_argument("Readable security observation requires nonzero process and thread IDs");
    }
    Json result = {
        {"schemaVersion", AuditSchemaVersion(message, c_observationSchemaVersion)},
        {"eventType", schema.eventType},
        {"family", family},
        {"operation", operation},
        {"outcome", message.at("outcome")}};
    const auto observed = [](const Json& value, ObservationType type, std::string_view state) -> Json {
        if (state != "available")
        {
            return {{"value", nullptr}, {"status", state}};
        }
        if (value.is_null())
        {
            return {{"value", nullptr}, {"status", "unavailable"}};
        }
        if (type == ObservationType::Text && value.get_ref<const std::string&>().find('\0') != std::string::npos)
        {
            throw std::invalid_argument("Security text cannot contain NUL");
        }
        if (type == ObservationType::Addresses)
        {
            for (const auto& address : value)
            {
                if (address.get_ref<const std::string&>().empty() || address.get_ref<const std::string&>().find('\0') != std::string::npos)
                {
                    throw std::invalid_argument("Invalid resolver address text");
                }
            }
        }
        Json encoded = value;
        if (type == ObservationType::Unsigned64)
        {
            encoded = std::to_string(value.get<uint64_t>());
        }
        else if (type == ObservationType::Signed64)
        {
            encoded = std::to_string(value.get<int64_t>());
        }
        return {{"value", std::move(encoded)}, {"status", "present"}};
    };
    for (const auto& [name, value] : message.at("fields").items())
    {
        if (ObservationFieldState(message, name) != "available" && !value.is_null())
        {
            throw std::invalid_argument("Value supplied for an inapplicable or unsupported wire field: " + name);
        }
    }
    for (const auto& field : ObservationCommonFields(operation))
    {
        const std::string_view name(field.name);
        const auto source = name == "vmPid"          ? "pid"
                            : name == "vmTid"        ? "tid"
                            : name == "linuxEventNs" ? "timestampNs"
                                                     : field.name;
        auto value = message.at(source);
        std::string_view state = "available";
        const bool credential = name == "uid" || name == "euid" || name == "gid" || name == "egid";
        if ((credential && value == UINT32_MAX) || ((name == "euid" || name == "egid") && value == 0) ||
            ((name.ends_with("Namespace") || name.ends_with("Ns") || name == "cgroupId") && value == 0))
        {
            // Generic syscall enrichment cannot distinguish failed effective-ID reads from real root zero.
            value = nullptr;
        }
        result[field.name] = observed(value, field.type, state);
        if (name == "linuxEventNs")
        {
            result[field.name]["clock"] = "monotonic";
        }
    }
    for (const auto& field : ObservationFields(family, operation))
    {
        const std::string_view name(field.name);
        auto value = message.at("fields").at(field.name);
        const auto state = ObservationFieldState(message, name);
        if (state != "available" && !value.is_null())
        {
            throw std::invalid_argument(std::string("Value supplied for inapplicable or unsupported field: ") + field.name);
        }
        if ((name == "socketInode" || name.ends_with("Namespace")) && value == 0)
        {
            value = nullptr;
        }
        if ((name == "oldUid" || name == "oldGid" || name == "newUid" || name == "newGid") && value == UINT32_MAX)
        {
            value = nullptr;
        }
        if (field.type == ObservationType::Text && value == "")
        {
            value = nullptr;
        }
        result[field.name] = observed(value, field.type, state);
    }
    return result;
}

} // namespace wsl::audit
