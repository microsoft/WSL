/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WsladiagTests.cpp

Abstract:

    This file contains smoke tests for wsladiag.

--*/

#include "precomp.h"
#include "Common.h"
#include <format>

namespace WsladiagTests {
class WsladiagTests
{
    WSL_TEST_CLASS(WsladiagTests)

    // Test that wsladiag list command shows either sessions or "no sessions" message
    TEST_METHOD(List_ShowsSessionsOrNoSessions)
    {
        auto [out, err, code] = RunWsladiag(L"list");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", err);

        ValidateListOutput(out);
    }

    // Test that wsladiag --help shows usage information
    TEST_METHOD(Help_ShowsUsage)
    {
        ValidateWslaDiagOutput(L"--help", 0, L"Usage:");
    }

    // Test that wsladiag with no arguments shows usage information
    TEST_METHOD(EmptyCommand_ShowsUsage)
    {
        ValidateWslaDiagOutput(L"", 0, L"Usage:");
    }

    // Test that -h and --help flags produce identical output
    TEST_METHOD(Help_ShortAndLongFlags_Match)
    {
        auto [outH, errH, codeH] = RunWsladiag(L"-h");
        auto [outLong, errLong, codeLong] = RunWsladiag(L"--help");

        VERIFY_ARE_EQUAL(0, codeH);
        VERIFY_ARE_EQUAL(0, codeLong);

        VERIFY_ARE_EQUAL(L"", outH);
        VERIFY_ARE_EQUAL(L"", outLong);

        VERIFY_ARE_EQUAL(errH, errLong);
        ValidateUsage(errH);
    }

    // Test that unknown commands show error message and usage
    TEST_METHOD(UnknownCommand_ShowsError)
    {
        auto [out, err, code] = RunWsladiag(L"blah");
        VERIFY_ARE_EQUAL(1, code);

        const std::wstring combined = out + err;
        VERIFY_IS_TRUE(combined.find(L"Unknown command: 'blah'") != std::wstring::npos);
        VERIFY_IS_TRUE(combined.find(L"Usage:") != std::wstring::npos);
    }

    // Test that shell command without session name shows error
    TEST_METHOD(Shell_MissingName_ShowsError)
    {
        ValidateWslaDiagFailsWith(L"shell", L"wsladiag shell <SessionName> [--verbose]");
    }

    // Test shell command with invalid session name (silent mode)
    TEST_METHOD(Shell_InvalidSessionName_Silent)
    {
        ValidateWslaDiagFailsWith(L"shell DefinitelyNotARealSession", L"Session not found: 'DefinitelyNotARealSession'");
    }

    // Test shell command with invalid session name (verbose mode)
    TEST_METHOD(Shell_InvalidSessionName_Verbose)
    {
        const std::wstring name = L"DefinitelyNotARealSession";
        auto [out, err, code] = RunWsladiag(std::format(L"shell {} --verbose", name));
        VERIFY_ARE_NOT_EQUAL(0, code);

        VERIFY_IS_TRUE(out.find(std::format(L"[diag] shell='{}'", name)) != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"Session not found") != std::wstring::npos);
    }

    // Build command line for wsladiag.exe with given arguments
    static std::wstring BuildWsladiagCmd(const std::wstring& args)
    {
        const auto msiPathOpt = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(msiPathOpt.has_value());

        const auto exePath = std::filesystem::path(*msiPathOpt) / L"wsladiag.exe";
        const auto exe = exePath.wstring();

        return args.empty() ? std::format(L"\"{}\"", exe) : std::format(L"\"{}\" {}", exe, args);
    }

    // Execute wsladiag with given arguments and return output, error, and exit code
    static std::tuple<std::wstring, std::wstring, int> RunWsladiag(const std::wstring& args)
    {
        auto cmd = BuildWsladiagCmd(args);
        return LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data());
    }

    static void ValidateWslaDiagOutput(const std::wstring& cmd, const std::wstring& expectedSubstring)
    {
        auto [out, err, code] = RunWsladiag(cmd);
        const std::wstring combined = out + err;
        VERIFY_IS_TRUE(combined.find(expectedSubstring) != std::wstring::npos);
    }

    static void ValidateWslaDiagOutput(const std::wstring& cmd, int expectedExitCode, const std::wstring& expectedSubstring)
    {
        auto [out, err, code] = RunWsladiag(cmd);
        VERIFY_ARE_EQUAL(expectedExitCode, code);

        const std::wstring combined = out + err;
        VERIFY_IS_TRUE(combined.find(expectedSubstring) != std::wstring::npos);
    }

    static void ValidateWslaDiagFailsWith(const std::wstring& cmd, const std::wstring& expectedSubstring)
    {
        auto [out, err, code] = RunWsladiag(cmd);
        VERIFY_ARE_NOT_EQUAL(0, code);

        const std::wstring combined = out + err;
        VERIFY_IS_TRUE(combined.find(expectedSubstring) != std::wstring::npos);
    }

    // Validate that list command output shows either no sessions message or session table
    static void ValidateListOutput(const std::wstring& out)
    {
        const bool noSessions = out.find(L"No WSLA sessions found.") != std::wstring::npos;

        const bool hasTable = out.find(L"WSLA session") != std::wstring::npos && out.find(L"ID") != std::wstring::npos &&
                              out.find(L"Creator PID") != std::wstring::npos && out.find(L"Display Name") != std::wstring::npos;

        VERIFY_IS_TRUE(noSessions || hasTable);
    }

    // Validate that usage information contains expected command descriptions
    static void ValidateUsage(const std::wstring& err)
    {
        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag list") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag shell <SessionName> [--verbose]") != std::wstring::npos);
    }
};
} // namespace WsladiagTests