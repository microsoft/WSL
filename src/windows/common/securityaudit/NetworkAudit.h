// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include <string>
#include <string_view>
#include <gsl/gsl>

namespace wsl::windows::common::securityaudit {

class EventSource;

std::wstring FormatNetworkAudit(gsl::span<gsl::byte> bytes);
std::wstring FormatNetworkAuditDisplay(LX_NETWORK_AUDIT_OPERATION operation, std::wstring_view details);
void ReportNetworkAudit(EventSource& source, const LX_INIT_NETWORK_AUDIT& event, std::wstring_view details);

} // namespace wsl::windows::common::securityaudit
