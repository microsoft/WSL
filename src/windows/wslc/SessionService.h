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
