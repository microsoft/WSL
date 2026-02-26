/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.h

Abstract:

    This file contains the SessionService definition

--*/
#pragma once

#include "SessionModel.h"
#include <wslaservice.h>

namespace wsl::windows::wslc::services {
struct SessionService
{
    static wsl::windows::wslc::models::Session CreateSession(const std::optional<wsl::windows::wslc::models::SessionOptions>& options = std::nullopt);
};
} // namespace wsl::windows::wslc::services
