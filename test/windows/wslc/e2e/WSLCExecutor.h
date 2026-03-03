/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutor.h

Abstract:

    This file contains the declaration of the WSLCExecutor class, which
    provides functionality to execute wslc commands and verify their results in
    end-to-end tests.
--*/

#pragma once

#include "precomp.h"
#include "windows/Common.h"

namespace WSLCE2ETests {

struct WSLCExecutionResult
{
    std::wstring CommandLine{};
    std::optional<std::wstring> Stdout{};
    std::optional<std::wstring> Stderr{};
    std::optional<HRESULT> ExitCode{};
    void Dump() const;
    void Verify(const WSLCExecutionResult& expected) const;
    std::vector<std::wstring> GetStdoutLines() const;
    std::wstring GetStdoutOneLine() const;
};

WSLCExecutionResult RunWslc(const std::wstring& commandLine);
void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected);
std::wstring GetWslcHeader();
} // namespace WSLCE2ETests
