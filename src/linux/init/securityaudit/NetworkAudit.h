// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include <gsl/gsl>
#include <optional>

namespace wsl::linux::securityaudit {

// The collector validates the SDK envelope before dispatching records.
std::optional<LX_INIT_NETWORK_AUDIT> DecodeNetworkAudit(gsl::span<const gsl::byte> data);

} // namespace wsl::linux::securityaudit
