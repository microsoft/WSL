/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutorHelpers.h

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#pragma once

#include "WSLCExecutor.h"

namespace WSLCE2ETests {

std::wstring GetStdoutOneLine(const WSLCExecutionResult& result);
void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void VerifyContainerIsNotListed(const std::wstring& containerNameOrId);
void EnsureContainerDoesNotExist(const std::wstring& containerName);

} // namespace WSLCE2ETests