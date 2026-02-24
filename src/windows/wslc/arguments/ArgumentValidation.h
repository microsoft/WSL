/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ArgumentValidation.h

Abstract:

    Declaration of Argument Validation functions.

--*/
#pragma once
#include "ArgumentTypes.h"
#include "Exceptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc::validation {

template <typename T>
void ValidateIntegerFromString(const std::vector<std::wstring>& values, const std::wstring& argName);

template <typename T>
T GetIntegerFromString(const std::wstring& value, const std::wstring& argName = {});

} // namespace wsl::windows::wslc::validation