// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "SecurityEvents.h"

inline nlohmann::json SecurityFixture(std::string_view operation = "connect")
{
    using namespace wsl::audit;
    const auto family = SecurityFamily(operation);
    nlohmann::json event = {
        {"version", c_protocolVersion},
        {"kind", "security"},
        {"family", family},
        {"operation", operation},
        {"pid", 123U},
        {"tid", 124U},
        {"pidNamespace", 5678U},
        {"uid", 1000U},
        {"euid", 0U},
        {"gid", 1001U},
        {"egid", 0U},
        {"mountNamespace", 11U},
        {"networkNamespace", 12U},
        {"cgroupId", uint64_t{13}},
        {"timestampNs", UINT64_MAX},
        {"startTimeNs", uint64_t{9007199254740993}},
        {"collectionStatus", 0U},
        {"result", int64_t{0}},
        {"outcome", "success"},
        {"fields", EmptySecurityFields(family)}};
    if (operation == "process-created")
    {
        event["result"] = nullptr;
        event["tid"] = event["pid"];
        event["fields"]["parentPid"] = 42U;
        event["fields"]["childPid"] = event["pid"];
        event["fields"]["childTid"] = event["tid"];
        event["fields"]["namespacePid"] = 321U;
        event["fields"]["parentStartTimeNs"] = uint64_t{9007199254740001};
        event["fields"]["executable"] = "/usr/bin/creator";
        event["fields"]["cwd"] = "/workspace";
        event["fields"]["executableTruncated"] = false;
        event["fields"]["cwdTruncated"] = false;
    }
    if (operation == "dns-query")
    {
        event["fields"]["dnsStatus"] = 0U;
    }
    return event;
}
