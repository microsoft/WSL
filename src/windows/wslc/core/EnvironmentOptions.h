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

// An environment variable that maps onto an ArgType. Presence of the variable
// in the process environment is what counts; the value is ignored for Flag
// kinds and stored verbatim for Value kinds. To "turn off" an env-bound
// option, unset the variable.
struct EnvBinding
{
    const wchar_t* Name;
    ArgType Type;
};

// Many-to-one allowed: multiple env var names may bind to one ArgType.
constexpr EnvBinding c_envBindings[] = {
    {L"WSLC_CLI_DEBUG", ArgType::Debug},
    {L"NO_COLOR", ArgType::NoColor},
};

// Populates target for any ArgType in definedArgs not already set. Flags are
// set by presence of the env var; values are stored verbatim.
//
// Contract: never throws on user input or environment state.
void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs) noexcept;

} // namespace wsl::windows::wslc
