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
    // When true, the env var follows a "presence implies truthy" contract
    // (e.g. NO_COLOR per https://no-color.org): the variable being defined
    // disables the feature regardless of value, including empty (NO_COLOR=)
    // and explicit "0"/"false"/"no"/"off". Only meaningful for Flag kinds;
    // ignored for Value kinds.
    bool PresenceOnly = false;
};

// Many-to-one allowed: multiple env var names may bind to one ArgType
// (e.g. NO_COLOR and WSLC_CLI_NO_COLOR both map to ArgType::NoColor).
// The vendor-specific WSLC_CLI_NO_COLOR keeps truthy-gating so users can
// explicitly opt back in with WSLC_CLI_NO_COLOR=0; the spec-defined
// NO_COLOR is presence-only as required by https://no-color.org.
constexpr EnvBinding c_envBindings[] = {
    {L"WSLC_CLI_DEBUG", ArgType::Debug},
    {L"WSLC_CLI_NO_COLOR", ArgType::NoColor},
    {L"NO_COLOR", ArgType::NoColor, true},
};

// Populates target for any ArgType in definedArgs not already set. Flags use
// a truthy check (or presence-only check when the binding opts in); values
// store the env string.
//
// Contract: never throws on user input or environment state. The function
// runs before NO_COLOR is applied, so a throw here could surface as colored
// help/error output. The only path that fail-fasts is a programming-error
// configuration (an ArgType whose Kind cannot be env-bound); that is *not*
// reachable from user input.
void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs) noexcept;

} // namespace wsl::windows::wslc
