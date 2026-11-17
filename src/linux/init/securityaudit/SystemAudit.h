// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include <gsl/gsl>
#include <optional>

namespace wsl::linux::securityaudit {

// The collector validates the SDK envelope; unrelated event types and syscalls are ignored.
std::optional<LX_INIT_SYSTEM_AUDIT> DecodeSystemAudit(gsl::span<const gsl::byte> data);

} // namespace wsl::linux::securityaudit
