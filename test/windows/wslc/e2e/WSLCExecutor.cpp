/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ETests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;

void WSLCExecutionResult::Dump() const
{
    Log::Comment((L"Command Line: " + CommandLine).c_str());
    Log::Comment((L"Exit Code: " + std::to_wstring(ExitCode)).c_str());
    Log::Comment((L"Stdout: " + Stdout).c_str());
    Log::Comment((L"Stderr: " + Stderr).c_str());
}

void WSLCExecutionResult::Verify(const WSLCExecutionResult& expected) const
{
    VERIFY_ARE_EQUAL(expected.Stdout, Stdout, std::format(L"Stdout does not match expected for command '{}'", CommandLine).c_str());
    VERIFY_ARE_EQUAL(expected.Stderr, Stderr, std::format(L"Stderr does not match expected for command '{}'", CommandLine).c_str());
    VERIFY_ARE_EQUAL(expected.ExitCode, ExitCode, std::format(L"ExitCode does not match expected for command '{}'", CommandLine).c_str());
}

std::vector<std::wstring> WSLCExecutionResult::GetStdoutLines() const
{
    std::vector<std::wstring> lines;
    std::wstringstream ss(Stdout);
    std::wstring line;
    while (std::getline(ss, line))
    {
        // Remove carriage return if present
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }

        lines.push_back(line);
    }
    return lines;
}

WSLCExecutionResult WSLCExecutor::Execute(const std::wstring& commandLine)
{
    auto fullCmd = L"C:\\src\\wsl\\bin\\x64\\Debug\\wslc.exe " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, fullCmd.c_str());
    const auto output = process.RunAndCaptureOutput();
    return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = HRESULT_FROM_WIN32(output.ExitCode)};
}

void WSLCExecutor::ExecuteAndVerify(const std::wstring& cmd, const std::wstring& expectedStdout, const std::wstring& expectedStderr, HRESULT expectedExitCode)
{
    WSLCExecutionResult expected{.Stdout = expectedStdout, .Stderr = expectedStderr, .ExitCode = expectedExitCode};
    ExecuteAndVerify(cmd, expected);
}

void WSLCExecutor::ExecuteAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected)
{
    Execute(cmd).Verify(expected);
}

std::wstring GetWslcHeader()
{
    std::wstringstream header;
    header << L"Windows Subsystem for Linux Container CLI (Preview) v1.0.0\r\n"
           << L"Copyright (c) Microsoft Corporation. All rights reserved.\r\n\r\n";
    return header.str();
}
} // namespace WSLCE2ETests