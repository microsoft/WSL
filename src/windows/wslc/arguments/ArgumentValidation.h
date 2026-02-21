/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.cpp

Abstract:

    Declaration of Argument Validation functions.

--*/
#pragma once
#include "ArgumentTypes.h"
#include "Exceptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc::validation {

void ValidateUInteger(const std::vector<std::wstring>& values, const std::wstring& argName);

} // namespace wsl::windows::wslc::validation