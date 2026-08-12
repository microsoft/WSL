/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    MountSpecParsing.h

Abstract:

    Declarations for parsing Docker-style --mount specifications.

--*/
#pragma once

#include <string>

namespace wsl::windows::wslc::validation {

struct ParsedMount
{
    bool IsTmpfs = false;
    std::wstring VolumeSpec;
    std::string TmpfsSpec;
};

// Parses a Docker-style --mount spec into the existing volume or tmpfs representation.
ParsedMount ParseMount(const std::wstring& value);

} // namespace wsl::windows::wslc::validation
