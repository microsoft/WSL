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

// Populates target from environment for any ArgType in definedArgs that is not
// already set. Flag args use a truthy check; Value args store the env string.
void ApplyEnvironmentOptions(argument::ArgMap& target, const std::vector<Argument>& definedArgs);

} // namespace wsl::windows::wslc
