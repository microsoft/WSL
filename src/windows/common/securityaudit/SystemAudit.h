// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include <gsl/gsl>
#include <string>
#include <string_view>

namespace wsl::windows::common::securityaudit {

class EventSource;

std::wstring FormatSystemAudit(gsl::span<gsl::byte> bytes);
std::wstring FormatSystemAuditDisplay(LX_SYSTEM_AUDIT_OPERATION operation, std::wstring_view details);
void ReportSystemAudit(EventSource& source, const LX_INIT_SYSTEM_AUDIT& event, std::wstring_view details);

} // namespace wsl::windows::common::securityaudit
