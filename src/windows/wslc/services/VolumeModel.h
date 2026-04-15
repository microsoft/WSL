/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeModel.h

Abstract:

    This file contains the VolumeModel definitions

--*/

#pragma once

#include "JsonUtils.h"
#include <string>

namespace wsl::windows::wslc::models {

struct VolumeInformation
{
    std::string Name;
    std::string Type;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(VolumeInformation, Name, Type);
};

} // namespace wsl::windows::wslc::models
