/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutorHelpers.h

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#pragma once

namespace WSLCE2ETests {

std::wstring GetStdoutOneLine(const WSLCExecutionResult& result);
void EnsureContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void EnsureContainerIsNotListed(const std::wstring& containerNameOrId);
void EnsureContainerDoesNotExist(const std::wstring& containerName);

} // namespace WSLCE2ETests