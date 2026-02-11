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

namespace wslc::services
{
class SessionService
{
public:
    wslc::models::Session CreateSession(std::optional<wslc::models::SessionOptions> options = std::nullopt);
};
}
