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

using namespace WEX::Logging;

struct WSLCExecutionResult
{
    std::wstring Stdout{};
    std::wstring Stderr{};
    HRESULT ExitCode{S_OK};
    void Dump() const
    {
        Log::Comment((L"Exit Code: " + std::to_wstring(ExitCode)).c_str());
        Log::Comment((L"Stdout: " + Stdout).c_str());
        Log::Comment((L"Stderr: " + Stderr).c_str());
    }
};

class WSLCE2ETests
{
    WSL_TEST_CLASS(WSLCE2ETests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_HelpCommand_DisplaysCopyrightInfo)
    {
        auto result = ExecuteWSLC(L"--help");
        auto expectedOutput = L"Copyright (c) Microsoft Corporation. All rights reserved.";
        WSLCExecutionResult expectedResult{.Stdout = expectedOutput};
        VerifyOutput(result, expectedResult);
    }

    TEST_METHOD(WSLCE2E_HelpCommand_DisplaysAvailableCommands)
    {
        auto result = ExecuteWSLC(L"--help");
        std::wstringstream expectedOutput;
        expectedOutput << L"The following commands are available:\r\n"
                       << L"  container  Container command\r\n"
                       << L"  session    Session command\r\n"
                       << L"  create     Create a container.\r\n"
                       << L"  delete     Delete containers\r\n"
                       << L"  exec       Execute a command in a running container.\r\n"
                       << L"  inspect    Inspect a container.\r\n"
                       << L"  kill       Kill containers\r\n"
                       << L"  list       List containers.\r\n"
                       << L"  run        Run a container.\r\n"
                       << L"  start      Start a container.\r\n"
                       << L"  stop       Stop containers\r\n";

        WSLCExecutionResult expectedResult{.Stdout = expectedOutput.str()};
        VerifyOutput(result, expectedResult);
    }

    TEST_METHOD(WSLCE2E_Container_HelpCommand_DisplaysAvailableSubCommands)
    {
        auto result = ExecuteWSLC(L"container --help");
        std::wstringstream expectedOutput;
        expectedOutput << L"The following sub-commands are available:\r\n"
                       << L"  create   Create a container.\r\n"
                       << L"  delete   Delete containers\r\n"
                       << L"  exec     Execute a command in a running container.\r\n"
                       << L"  inspect  Inspect a container.\r\n"
                       << L"  kill     Kill containers\r\n"
                       << L"  list     List containers.\r\n"
                       << L"  run      Run a container.\r\n"
                       << L"  start    Start a container.\r\n"
                       << L"  stop     Stop containers\r\n";
        WSLCExecutionResult expectedResult{.Stdout = expectedOutput.str()};
        VerifyOutput(result, expectedResult);
    }

    TEST_METHOD(WSLCE2E_InvalidCommand_DisplaysErrorMessage)
    {
        auto result = ExecuteWSLC(L"invalidcmd");
        auto expectedError = L"Unrecognized command: 'invalidcmd'";
        WSLCExecutionResult expectedResult{.Stderr = expectedError, .ExitCode = E_INVALIDARG};
        VerifyOutput(result, expectedResult);
    }

private:
    WSLCExecutionResult ExecuteWSLC(const std::wstring& cmd)
    {
        auto [read, write] = CreateSubprocessPipe(true, false);
        write.reset();

        auto fullCmd = L"C:\\src\\wsl\\bin\\x64\\Debug\\wslc.exe " + cmd;
        wsl::windows::common::SubProcess process(nullptr, fullCmd.c_str());
        process.SetStdHandles(read.get(), nullptr, nullptr);
        const auto output = process.RunAndCaptureOutput();
        return {.Stdout = output.Stdout, .Stderr = output.Stderr, .ExitCode = HRESULT_FROM_WIN32(output.ExitCode)};
    }

    void VerifyOutput(const WSLCExecutionResult& result, const WSLCExecutionResult& expected) const
    {
        VERIFY_IS_TRUE(result.Stdout.find(expected.Stdout) != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stderr.find(expected.Stderr) != std::wstring::npos);
        VERIFY_ARE_EQUAL(expected.ExitCode, result.ExitCode);
    }
};
} // namespace WSLCE2ETests