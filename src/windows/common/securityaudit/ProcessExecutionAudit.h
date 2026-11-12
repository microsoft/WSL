// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <gsl/gsl>
#include <guiddef.h>

namespace wsl::windows::common::securityaudit {

class EventSource;

struct ProcessExecutionDistribution
{
    GUID Id;
    std::wstring Name;
};

bool ProcessExecutionEnabled();
bool IncludeProcessCommandLine();
// Unmatched namespaces, including VM-root and nested namespaces, omit distribution fields.
std::wstring FormatProcessExecution(gsl::span<gsl::byte> bytes, bool includeCommandLine, const ProcessExecutionDistribution* distribution = nullptr);
void ReportProcessExecution(EventSource& source, uint32_t pid);
void ReportProcessExecutionDetails(EventSource& source, uint32_t pid, std::wstring_view details);

} // namespace wsl::windows::common::securityaudit
