// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "../../shared/securityaudit/Protocol.h"

#include <span>
#include <vector>

namespace wsl::audit {

enum class DecodeStatus
{
    Process,
    Security,
    Ignored,
    Invalid
};

struct DecodedRecord
{
    DecodeStatus status{DecodeStatus::Invalid};
    nlohmann::json message;
    std::string error;
};

struct CollectorSyscall
{
    int number;
    bool successOnly;
};

struct CollectionPlan
{
    std::vector<CollectorSyscall> syscalls;
    int features{};
    uint64_t runtimeFlags{};
};

CollectionPlan BuildCollectionPlan(const AuditPolicy& policy);
DecodedRecord DecodeRecord(std::span<const std::byte> data, bool includeCommandLine);
bool PrepareCollectorTracefs();
bool MatchesContainerCgroup(std::string_view cgroups, std::string_view containerId);
nlohmann::json InspectProcessNamespaces(uint32_t pid, std::string_view containerId = {});
int RunCollector(int inputFd, int outputFd);

} // namespace wsl::audit
