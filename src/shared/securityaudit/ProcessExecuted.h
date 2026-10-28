// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "Protocol.h"

namespace wsl::audit {

inline constexpr uint32_t c_processExecutedEventId = 62010;
inline constexpr uint32_t c_processExecutedSchemaVersion = 4;

inline nlohmann::json ProcessExecutedData(nlohmann::json message, bool includeCommandLine)
{
    using Json = nlohmann::json;
    if (!includeCommandLine)
    {
        message.erase("argv");
    }
    ValidateProcessEvent(message);
    for (const auto* name : c_processBooleanFields)
    {
        if (!message.contains(name))
        {
            throw std::invalid_argument(std::string("ProcessExecuted requires collection metadata: ") + name);
        }
    }
    if (message.at("pid") == 0 || message.at("tid") == 0)
    {
        throw std::invalid_argument("ProcessExecuted requires nonzero VM PID and TID");
    }
    const auto observed = [](Json value, bool missing) -> Json {
        return {{"value", missing ? Json(nullptr) : std::move(value)}, {"status", missing ? "unavailable" : "present"}};
    };
    Json data{
        {"schemaVersion", AuditSchemaVersion(message, c_processExecutedSchemaVersion)},
        {"eventType", "ProcessExecuted"},
        {"outcome", "success"},
        {"vmPid", message.at("pid")},
        {"vmTid", message.at("tid")},
        {"vmParentPid", observed(message.at("ppid"), message.at("ppid") == 0)},
        {"pidNamespace",
         observed(message.at("pidNamespace"), message.at("pidNamespaceMissing").get<bool>() || message.at("pidNamespace") == 0)}};
    // PM Sheet1 C2/C4: these optional transport values are supplied by runtime container metadata.
    for (const auto* name : {"namespacePid", "mountNamespace", "networkNamespace", "cgroupId"})
    {
        const auto found = message.find(name);
        const bool missing = found == message.end() || found->is_null() || *found == 0;
        Json value = missing ? Json(nullptr) : *found;
        if (!missing && std::string_view(name) == "cgroupId")
        {
            value = std::to_string(found->get<uint64_t>());
        }
        data[name] = observed(std::move(value), missing);
    }
    for (const auto* name : {"uid", "euid", "gid", "egid"})
    {
        data[name] = observed(message.at(name), message.at(std::string(name) + "Missing").get<bool>() || message.at(name) == UINT32_MAX);
    }
    for (const auto* name : {"executable", "cwd"})
    {
        const bool missing =
            message.at(std::string(name) + "Missing").get<bool>() || message.at(name).get_ref<const std::string&>().empty();
        data[name] = observed(message.at(name), missing);
        data[name]["truncated"] = !missing && message.at(std::string(name) + "Truncated").get<bool>();
    }
    const auto clock = [&](const char* name, const char* missingFlag, const char* clockName) {
        const auto value = message.at(name).get<uint64_t>();
        // Decimal strings preserve nanoseconds in consumers that use IEEE-754 JSON numbers.
        auto field = observed(std::to_string(value), message.at(missingFlag).get<bool>() || value == 0);
        field["clock"] = clockName;
        return field;
    };
    data["linuxEventNs"] = clock("timestampNs", "timestampMissing", "monotonic");
    const bool missingArguments = !message.contains("argv") || message.at("cmdlineMissing").get<bool>();
    data["argv"] = observed(includeCommandLine && !missingArguments ? message.at("argv") : Json(nullptr), missingArguments);
    if (!includeCommandLine)
    {
        data["argv"] = {{"value", nullptr}, {"status", "disabled_by_policy"}};
    }
    data["argv"]["truncated"] = includeCommandLine && !missingArguments && message.at("cmdlineTruncated").get<bool>();
    // PM C2 deferred declarations: enable only after adding a reliable source, decoder, versioned reader and tests.
    // data["capabilities"] = ...; // No capability snapshot in the selected exec producer.
    // data["hash"] = ...;         // No executable hash in the selected producer.
    // data["hostDeviceId"] = ...; // Windows device identity contract is not defined.
    // data["wslDeviceId"] = ...;  // WSL device identity contract is not defined.
    // A successful exec notification is not a process exit code or a failed-exec result.
    return data;
}

} // namespace wsl::audit
