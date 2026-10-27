// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string>

namespace wsl::windows::wslc::models {

struct ComposeProjectInformation
{
    std::string Name;
    std::string Status;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ComposeProjectInformation, Name, Status);
};

} // namespace wsl::windows::wslc::models
