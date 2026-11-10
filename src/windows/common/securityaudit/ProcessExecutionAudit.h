// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <gsl/gsl>

namespace wsl::windows::common::securityaudit {

class EventSource;

bool ProcessExecutionEnabled();
bool IncludeProcessCommandLine();
std::wstring FormatProcessExecution(gsl::span<gsl::byte> bytes, bool includeCommandLine);
void ReportProcessExecution(EventSource& source, uint32_t pid);
void ReportProcessExecutionDetails(EventSource& source, uint32_t pid, std::wstring_view details);

} // namespace wsl::windows::common::securityaudit
