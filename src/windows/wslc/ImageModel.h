/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageModel.h

Abstract:

    This file contains the ImageModel definition.

--*/
#pragma once

#include <docker_schema.h>

namespace wsl::windows::wslc::models {
struct ImageInformation
{
    std::string Name;
    ULONGLONG Size;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ImageInformation, Name, Size);
};
} // namespace wsl::windows::wslc::models