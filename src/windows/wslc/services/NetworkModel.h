/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkModel.h

Abstract:

    This file contains the NetworkModel definitions

--*/

#pragma once

#include "JsonUtils.h"
#include <string>

namespace wsl::windows::wslc::models {

struct CreateNetworkOptions
{
    std::string Name;
    std::optional<std::string> Driver;
    std::vector<std::pair<std::string, std::string>> DriverOpts{};
    std::vector<std::pair<std::string, std::string>> Labels{};
};

} // namespace wsl::windows::wslc::models
