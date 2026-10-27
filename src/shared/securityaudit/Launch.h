// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>

namespace wsl::audit {

inline constexpr uint32_t c_nativeAuditLaunchPort = 50007;
inline constexpr char c_nativeAuditCollectorPath[] = "/init";
inline constexpr char c_nativeAuditCollectorName[] = "wsl-audit-collector";

} // namespace wsl::audit
