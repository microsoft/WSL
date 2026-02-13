/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Declaration of Argument Validation functions.

--*/
#pragma once
#include "pch.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc::validation {

void ValidatePublish([[maybe_unused]] const ArgType argType, const ArgMap& execArgs);

} // namespace wsl::windows::wslc::validation