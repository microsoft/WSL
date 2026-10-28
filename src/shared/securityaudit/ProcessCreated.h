// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "SecurityEvents.h"

namespace wsl::audit {

inline constexpr uint32_t c_processCreatedEventId = 62011;
inline constexpr uint32_t c_processCreatedSchemaVersion = 3;

inline nlohmann::json ProcessCreatedData(const nlohmann::json& message)
{
    using Json = nlohmann::json;
    ValidateSecurityEvent(message);
    const auto& fields = message.at("fields");
    if (message.at("operation") != "process-created" || message.at("pid") == 0 || message.at("pid") != message.at("tid") ||
        fields.at("childPid") != message.at("pid") || fields.at("childTid") != message.at("tid"))
    {
        throw std::invalid_argument("ProcessCreated requires a nonzero new process leader and consistent child identity");
    }
    const auto observed = [](Json value, bool missing) -> Json {
        return {{"value", missing ? Json(nullptr) : std::move(value)}, {"status", missing ? "unavailable" : "present"}};
    };
    const auto identifier = [&](const Json& value) { return observed(value, value.is_null() || value == 0); };
    Json data{
        {"schemaVersion", AuditSchemaVersion(message, c_processCreatedSchemaVersion)},
        {"eventType", "ProcessCreated"},
        {"outcome", "success"},
        {"vmPid", message.at("pid")},
        {"vmTid", message.at("tid")},
        {"vmCreatorPid", identifier(fields.at("parentPid"))},
        {"pidNamespace", identifier(message.at("pidNamespace"))},
        {"namespacePid", identifier(fields.at("namespacePid"))},
        {"mountNamespace", identifier(message.at("mountNamespace"))},
        {"networkNamespace", identifier(message.at("networkNamespace"))}};
    // populate_container_runtime_metadata samples the current creator's cgroup, not the child's.
    const auto& cgroup = message.at("cgroupId");
    data["creatorCgroupId"] = observed(
        cgroup.is_null() || cgroup == 0 ? Json(nullptr) : Json(std::to_string(cgroup.get<uint64_t>())), cgroup.is_null() || cgroup == 0);
    for (const auto* name : {"uid", "euid", "gid", "egid"})
    {
        const auto& value = message.at(name);
        data[name] = observed(value, value.is_null() || value == UINT32_MAX);
    }
    const auto clock = [&](const Json& value, const char* clockName) {
        const bool missing = value.is_null() || value == 0;
        auto field = observed(missing ? Json(nullptr) : Json(std::to_string(value.get<uint64_t>())), missing);
        field["clock"] = clockName;
        return field;
    };
    data["linuxEventNs"] = clock(message.at("timestampNs"), "monotonic");
    // PM C4 deferred declarations: decoder, reader/version and tests must accompany activation.
    // data["parentTid"] = ...;    // The enriched fork ABI supplies creator TGID, not creator TID.
    // data["realParentPid"] = ...;// Creator is not necessarily real parent under CLONE_PARENT.
    // data["cloneFlags"] = ...;   // Synthetic fork argument slots are not reliable flags.
    // data["childCgroupId"] = ...;// bpf_get_current_cgroup_id() describes the creator.
    return data;
}

} // namespace wsl::audit
