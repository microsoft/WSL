/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    EnvironmentOptions.h

Abstract:

    Maps environment variables onto ArgTypes for early CLI configuration.

--*/
#pragma once
#include "Argument.h"
#include "ArgumentTypes.h"

#include <vector>

namespace wsl::windows::wslc {

struct EnvBinding
{
    const wchar_t* Name;
    ArgType Type;
};

// Source of truth for env-var to ArgType mapping. Many-to-one allowed:
// alternate spellings for the same ArgType (e.g. NO_COLOR and WSLC_CLI_NO_COLOR).
constexpr EnvBinding c_envBindings[] = {
    {L"WSLC_CLI_DEBUG", ArgType::Debug},
    {L"WSLC_CLI_NO_COLOR", ArgType::NoColor},
    {L"NO_COLOR", ArgType::NoColor},
};

// Populates target from environment for any ArgType in definedArgs that is not
// already set. Flag args use a truthy check; Value args store the env string.
void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs);

} // namespace wsl::windows::wslc
