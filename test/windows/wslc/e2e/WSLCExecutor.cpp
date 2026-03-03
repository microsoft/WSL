/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutor.cpp

Abstract:

    This file contains the implementation of the WSLCExecutor class, which is
    responsible for executing wslc commands and verifying their results in
    end-to-end tests.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

void WSLCExecutionResult::Dump() const
{
    Log::Comment((L"Command Line: \"" + CommandLine + L"\"").c_str());
    if (Stdout)
    {
        Log::Comment((L"Stdout: \"" + *Stdout + L"\"").c_str());
    }

    if (Stderr)
    {
        Log::Comment((L"Stderr: \"" + *Stderr + L"\"").c_str());
    }

    if (ExitCode)
    {
        Log::Comment((L"Exit Code: " + std::to_wstring(*ExitCode)).c_str());
    }
}

void WSLCExecutionResult::Verify(const WSLCExecutionResult& expected) const
{
    if (expected.Stdout)
    {
        VERIFY_ARE_EQUAL(*expected.Stdout, *Stdout);
    }

    if (expected.Stderr)
    {
        VERIFY_ARE_EQUAL(*expected.Stderr, *Stderr);
    }

    if (expected.ExitCode)
    {
        VERIFY_ARE_EQUAL(*expected.ExitCode, *ExitCode);
    }
}

std::vector<std::wstring> WSLCExecutionResult::GetStdoutLines() const
{
    std::vector<std::wstring> lines;
    std::wstringstream ss(*Stdout);
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

std::wstring WSLCExecutionResult::GetStdoutOneLine() const
{
    auto stdoutLines = GetStdoutLines();
    VERIFY_ARE_EQUAL(1u, stdoutLines.size());
    return stdoutLines[0];
}

WSLCExecutionResult RunWslc(const std::wstring& commandLine)
{
    auto wslcPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"wslc.exe";
    auto cmd = L"\"" + wslcPath.wstring() + L"\" " + commandLine;
    wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
    const auto output = process.RunAndCaptureOutput();
    return {.CommandLine = commandLine, .Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = static_cast<HRESULT>(output.ExitCode)};
}

void RunWslcAndVerify(const std::wstring& cmd, const WSLCExecutionResult& expected)
{
    RunWslc(cmd).Verify(expected);
}

std::wstring GetWslcHeader()
{
    std::wstringstream header;
    header << L"Windows Subsystem for Linux Container CLI (Preview) v1.0.0\r\n"
           << L"Copyright (c) Microsoft Corporation. All rights reserved.\r\n\r\n";
    return header.str();
}
} // namespace WSLCE2ETests