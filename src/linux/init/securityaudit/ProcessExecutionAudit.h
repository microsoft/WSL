// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "lxinitshared.h"
#include "message.h"
#include <optional>

namespace wsl::linux::securityaudit {

// The collector validates the common SDK envelope before dispatching here.
// Unenriched records are ignored; malformed payloads throw.
std::optional<wsl::shared::MessageWriter<LX_INIT_PROCESS_EXECUTED_DETAILS>> DecodeProcessExecution(gsl::span<const gsl::byte> data, bool includeCommandLine);

} // namespace wsl::linux::securityaudit
