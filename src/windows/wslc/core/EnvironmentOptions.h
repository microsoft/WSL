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

// Binding contract: presence of Name sets the option. The value is ignored
// for Flag kinds and stored verbatim for Value kinds. To opt out, unset the
// variable.
struct EnvBinding
{
    const wchar_t* Name;
    ArgType Type;
};

// Many-to-one allowed: multiple env var names may bind to one ArgType.
constexpr EnvBinding c_envBindings[] = {
    {L"NO_COLOR", ArgType::NoColor},
};

// Populates target for any ArgType in definedArgs not already set.
// Never throws on user input or environment state.
void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs) noexcept;

} // namespace wsl::windows::wslc
