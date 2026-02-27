/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ETests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

namespace WSLCE2ETests {

struct WSLCExecutionResult
{
    std::wstring CommandLine{};
    std::wstring Stdout{};
    std::wstring Stderr{};
    HRESULT ExitCode{S_OK};
    void Dump() const;
    void Verify(const WSLCExecutionResult& expected) const;
};

struct WSLCExecutor
{
    static WSLCExecutionResult Execute(const std::wstring& commandLine);
    static void ExecuteAndVerify(
        const std::wstring& cmd, const std::wstring& expectedStdout, const std::wstring& expectedStderr = L"", HRESULT expectedExitCode = S_OK);
    static void ExecuteAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected);
};

std::wstring GetWslcHeader();
} // namespace WSLCE2ETests
