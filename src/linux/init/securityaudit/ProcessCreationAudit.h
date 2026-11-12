// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include <gsl/gsl>
#include <optional>

namespace wsl::linux::securityaudit {

// The collector validates the SDK envelope; this decoder handles enriched process-creation records.
std::optional<LX_INIT_PROCESS_CREATED> DecodeProcessCreation(gsl::span<const gsl::byte> data);

} // namespace wsl::linux::securityaudit
