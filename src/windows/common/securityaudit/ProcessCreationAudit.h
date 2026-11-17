// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <gsl/gsl>

namespace wsl::windows::common::securityaudit {

class EventSource;

std::wstring FormatProcessCreation(gsl::span<gsl::byte> bytes);
std::wstring FormatProcessCreationDisplay(std::wstring_view details);
void ReportProcessCreation(EventSource& source, uint32_t childPid, std::wstring_view details);

} // namespace wsl::windows::common::securityaudit
