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

namespace WsladiagTests {
class WsladiagTests
{
    WSL_TEST_CLASS(WsladiagTests)

    // Use localized usage text at runtime
    static std::wstring GetUsageText()
    {
        return std::wstring(wsl::shared::Localization::MessageWsladiagUsage());
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

        ValidateListShowsSessionTable(stdOut);
    }

    // Test that wsladiag --help shows usage information
    TEST_METHOD(Help_ShowsUsage)
    {
        ValidateWsladiagOutput(L"--help", 0, L"", GetUsageText() + L"\r\n");
    }

    // Test that -h shows usage information
    TEST_METHOD(Help_ShortFlag_ShowsUsage)
    {
        ValidateWsladiagOutput(L"-h", 0, L"", GetUsageText() + L"\r\n");
    }

    // Test that wsladiag with no arguments shows usage information
    TEST_METHOD(EmptyCommand_ShowsUsage)
    {
        ValidateWsladiagOutput(L"", 0, L"", GetUsageText() + L"\r\n");
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

        VERIFY_IS_TRUE(stdErr.find(errorMsg) != std::wstring::npos);
        VERIFY_IS_TRUE(stdErr.find(usage) != std::wstring::npos);
    }

    // Test that shell command without session name shows error
    TEST_METHOD(Shell_MissingName_ShowsError)
    {
        auto [out, err, code] = RunWsladiag(L"shell");
        VERIFY_ARE_EQUAL(1, code);
        VERIFY_ARE_EQUAL(L"", out);

        const std::wstring missingArgMsg =
            std::wstring(wsl::shared::Localization::MessageMissingArgument(L"<SessionName>", L"wsladiag shell"));
        VERIFY_IS_TRUE(err.find(missingArgMsg) != std::wstring::npos);
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

    // Execute wsladiag with given arguments and return output, error, and exit code
    static std::tuple<std::wstring, std::wstring, int> RunWsladiag(const std::wstring& args)
    {
        std::wstring commandLine = BuildWsladiagCmd(args);
        return LxsstuLaunchCommandAndCaptureOutputWithResult(commandLine.data());
    }

    // Validate that list command output shows a session table
    static void ValidateListShowsSessionTable(const std::wstring& out)
    {
        const auto idHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderId());
        const auto pidHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderCreatorPid());
        const auto nameHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderDisplayName());

        VERIFY_IS_TRUE(out.find(idHeader) != std::wstring::npos);
        VERIFY_IS_TRUE(out.find(pidHeader) != std::wstring::npos);
        VERIFY_IS_TRUE(out.find(nameHeader) != std::wstring::npos);
    }

    static std::wstring BuildWsladiagCmd(const std::wstring& args)
    {
        const auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        const auto exePath = std::filesystem::path(*installPath) / L"wsladiag.exe";
        const auto exe = exePath.wstring();

        return args.empty() ? std::format(L"\"{}\"", exe) : std::format(L"\"{}\" {}", exe, args);
    }

    static void ValidateWsladiagOutput(const std::wstring& args, int expectedExitCode, const std::wstring& expectedStdout, const std::wstring& expectedStderr)
    {
        auto [stdOut, stdErr, exitCode] = RunWsladiag(args);
        VERIFY_ARE_EQUAL(expectedExitCode, exitCode);
        VERIFY_ARE_EQUAL(expectedStdout, stdOut);
        VERIFY_ARE_EQUAL(expectedStderr, stdErr);
    }
};
} // namespace WsladiagTests