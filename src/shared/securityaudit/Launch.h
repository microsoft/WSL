// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>

// The boot marker starts the child; its fixed policy arrives over the VM-bound socket.
#define WSL_SECURITY_AUDIT_ENV "WSL_SECURITY_AUDIT"

namespace wsl::audit {

inline constexpr uint32_t c_nativeAuditPort = 50007;
inline constexpr char c_nativeAuditCollectorName[] = "wsl-audit-collector";
inline constexpr char c_nativeAuditPackagePath[] = "/wsl-security-audit";

} // namespace wsl::audit
