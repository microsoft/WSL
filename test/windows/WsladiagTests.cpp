/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WsladiagTests.cpp

Abstract:

    This file contains smoke tests for wsladiag.

--*/

#include "precomp.h"
#include "Common.h"
#include "Localization.h"

#include <format>
#include <filesystem>

namespace WsladiagTests {
class WsladiagTests
{
    WSL_TEST_CLASS(WsladiagTests)

    // Add CRLFs for exact-string comparisons
    static std::wstring AddCrlf(const std::wstring& input)
    {
        std::wstring messageWithCrlf;

        for (const auto ch : input)
        {
            if (ch == L'\n')
            {
                messageWithCrlf += L'\r';
            }
            messageWithCrlf += ch;
        }

        return messageWithCrlf;
    }

    static std::wstring GetUsageText()
    {
        auto usage = AddCrlf(std::wstring(wsl::shared::Localization::MessageWsladiagUsage()));
        if (usage.empty() || usage.back() != L'\n')
        {
            usage += L"\r\n";
        }
        return usage;
    }

    static std::wstring BuildWsladiagCmd(const std::wstring& args)
    {
        const auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        const auto exePath = std::filesystem::path(*installPath) / L"wsladiag.exe";
        const auto exe = exePath.wstring();

        return args.empty() ? std::format(L"\"{}\"", exe) : std::format(L"\"{}\" {}", exe, args);
    }
    // Execute wsladiag with given arguments and return output, error, and exit code
    static std::tuple<std::wstring, std::wstring, int> RunWsladiag(const std::wstring& args)
    {
        std::wstring commandLine = BuildWsladiagCmd(args);
        return LxsstuLaunchCommandAndCaptureOutputWithResult(commandLine.data());
    }

    static void ValidateWsladiagOutput(const std::wstring& args, int expectedExitCode, const std::wstring& expectedStdout, const std::wstring& expectedStderr)
    {
        auto [stdOut, stdErr, exitCode] = RunWsladiag(args);
        VERIFY_ARE_EQUAL(expectedExitCode, exitCode);
        VERIFY_ARE_EQUAL(expectedStdout, stdOut);
        VERIFY_ARE_EQUAL(expectedStderr, stdErr);
    }

    // Test that wsladiag list command "no sessions" message
    TEST_METHOD(List_NoSessions)
    {
        auto [stdOut, stdErr, exitCode] = RunWsladiag(L"list");

        VERIFY_ARE_EQUAL(0, exitCode);
        VERIFY_ARE_EQUAL(L"", stdErr);
        VERIFY_ARE_EQUAL(std::wstring(wsl::shared::Localization::MessageWslaNoSessionsFound()) + L"\r\n", stdOut);
    }

    TEST_METHOD(List_ShowsSessions)
    {
        auto [stdOut, stdErr, exitCode] = RunWsladiag(L"list");

        VERIFY_ARE_EQUAL(0, exitCode);
        VERIFY_ARE_EQUAL(L"", stdErr);

        const std::wstring noSessions = std::wstring(wsl::shared::Localization::MessageWslaNoSessionsFound()) + L"\r\n";

        if (stdOut == noSessions)
        {
            return;
        }

        VERIFY_IS_TRUE(stdOut.find(L"ID") != std::wstring::npos);
        VERIFY_IS_TRUE(stdOut.find(L"Creator PID") != std::wstring::npos);
        VERIFY_IS_TRUE(stdOut.find(L"Display Name") != std::wstring::npos);
    }

    // Test that wsladiag --help shows usage information
    TEST_METHOD(Help_ShowsUsage)
    {
        ValidateWsladiagOutput(L"--help", 0, L"", GetUsageText());
    }

    // Test that -h shows usage information
    TEST_METHOD(Help_ShortFlag_ShowsUsage)
    {
        ValidateWsladiagOutput(L"-h", 0, L"", GetUsageText());
    }

    // Test that wsladiag with no arguments shows usage information
    TEST_METHOD(EmptyCommand_ShowsUsage)
    {
        ValidateWsladiagOutput(L"", 0, L"", GetUsageText());
    }

    // Test that unknown commands show error message and usage
    TEST_METHOD(UnknownCommand_ShowsError)
    {
        const std::wstring verb = L"blah";
        const std::wstring errorMsg = std::wstring(wsl::shared::Localization::MessageWslaUnknownCommand(verb.c_str()));
        const std::wstring usage = GetUsageText();

        auto [stdOut, stdErr, exitCode] = RunWsladiag(verb);

        VERIFY_ARE_EQUAL(1, exitCode);
        VERIFY_ARE_EQUAL(L"", stdOut);

        const std::wstring expected = errorMsg + L"\r\n" + usage;
        VERIFY_ARE_EQUAL(expected, stdErr);
    }

    // Test that shell command without session name shows error
    TEST_METHOD(Shell_MissingName_ShowsError)
    {
        auto [stdOut, stdErr, exitCode] = RunWsladiag(L"shell");

        VERIFY_ARE_EQUAL(1, exitCode);
        VERIFY_ARE_EQUAL(L"", stdOut);

        const std::wstring errorLine = L"Command line argument <SessionName> requires a value.";
        const std::wstring helpHint = L"Please use 'wsladiag shell --help' to get a list of supported arguments.";
        const std::wstring errorCode = L"Error code: E_INVALIDARG";

        const std::wstring expected = errorLine + L"\r\n" + helpHint + L"\r\n" + errorCode + L"\r\n";
        VERIFY_ARE_EQUAL(expected, stdErr);
    }

    // Test shell command with invalid session name (non verbose mode)
    TEST_METHOD(Shell_InvalidSessionName_NonVerbose)
    {
        const std::wstring name = L"DefinitelyNotARealSession";
        auto [stdOut, stdErr, exitCode] = RunWsladiag(L"shell " + name);

        VERIFY_ARE_EQUAL(1, exitCode);
        VERIFY_ARE_EQUAL(L"", stdOut);

        const auto expected = std::wstring(wsl::shared::Localization::MessageWslaSessionNotFound(name.c_str()));
        VERIFY_IS_TRUE(stdErr.find(expected) != std::wstring::npos);
    }

    // Test shell command with invalid session name (verbose mode)
    TEST_METHOD(Shell_InvalidSessionName_Verbose)
    {
        const std::wstring name = L"DefinitelyNotARealSession";
        auto [stdOut, stdErr, exitCode] = RunWsladiag(std::format(L"shell {} --verbose", name));

        VERIFY_ARE_EQUAL(1, exitCode);
        VERIFY_ARE_EQUAL(L"", stdOut);

        const auto expected = std::wstring(wsl::shared::Localization::MessageWslaSessionNotFound(name.c_str()));
        VERIFY_IS_TRUE(stdErr.find(expected) != std::wstring::npos);
    }
};
} // namespace WsladiagTests