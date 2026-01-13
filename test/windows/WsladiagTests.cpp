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

<<<<<<< HEAD
    // Initialize the tests
    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }
=======
    // Use localized usage text at runtime
    static std::wstring GetUsageText()
    {
        return std::wstring(wsl::shared::Localization::MessageWsladiagUsage());
    }

>>>>>>> ea162030 (Localize Wsladiag tests)
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
<<<<<<< HEAD
        auto [out, err, code] = RunWsladiag(L"--help");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);
        ValidateUsage(err);

=======
        ValidateWsladiagOutput(L"--help", 0, L"", GetUsageText());
>>>>>>> ea162030 (Localize Wsladiag tests)
    }

    // Test that wsladiag with no arguments shows usage information
    TEST_METHOD(EmptyCommand_ShowsUsage)
    {
<<<<<<< HEAD
        auto [out, err, code] = RunWsladiag(L"");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);
        ValidateUsage(err);
=======
        ValidateWsladiagOutput(L"", 0, L"", GetUsageText());
>>>>>>> ea162030 (Localize Wsladiag tests)
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
    TEST_METHOD(UnknownCommand_ShowsUsage)
    {
<<<<<<< HEAD

        auto [out, err, code] = RunWsladiag(L"blah");
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Unknown command: 'blah'") != std::wstring::npos);
        ValidateUsage(err);

=======
        const std::wstring verb = L"blah";
        const std::wstring errorMsg = std::wstring(wsl::shared::Localization::MessageWslaUnknownCommand(verb.c_str()));
        const std::wstring expected = errorMsg + GetUsageText();
        ValidateWsladiagOutput(verb, 1, L"", expected);
>>>>>>> ea162030 (Localize Wsladiag tests)
    }

    // Test that shell command without session name shows usage
    TEST_METHOD(Shell_MissingName_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"shell");
<<<<<<< HEAD
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);
        ValidateUsage(err);

=======
        VERIFY_ARE_EQUAL(1, code);
        VERIFY_ARE_EQUAL(L"", out);
        const std::wstring missingArgMsg =
            std::wstring(wsl::shared::Localization::MessageMissingArgument(L"<SessionName>", L"wsladiag shell"));
        VERIFY_IS_TRUE(NormalizeForCompare(err).find(NormalizeForCompare(missingArgMsg)) != std::wstring::npos);
>>>>>>> ea162030 (Localize Wsladiag tests)
    }

    // Test shell command with invalid session name (silent mode)
    TEST_METHOD(Shell_InvalidSessionName_Silent)
    {
<<<<<<< HEAD
        const std::wstring name = L"DefinitelyNotARealSession";
        auto [out, err, code] = RunWsladiag(std::format(L"shell {}", name));
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        ValidateSessionNotFound(err, name);
=======
        const auto expectedErr =
            std::wstring(wsl::shared::Localization::MessageWslaSessionNotFound(L"DefinitelyNotARealSession"));
        ValidateWsladiagOutput(L"shell DefinitelyNotARealSession", 1, L"", expectedErr);
>>>>>>> ea162030 (Localize Wsladiag tests)
    }

    // Test shell command with invalid session name (verbose mode)
    TEST_METHOD(Shell_InvalidSessionName_Verbose)
    {
        const std::wstring name = L"DefinitelyNotARealSession";
<<<<<<< HEAD
        auto [out, err, code] = RunWsladiag(std::format(L"shell {} --verbose", name));
        VERIFY_ARE_NOT_EQUAL(0, code);

        VERIFY_IS_TRUE(out.find(std::format(L"[diag] shell='{}'", name)) != std::wstring::npos);
        ValidateSessionNotFound(err, name);
=======
        const auto expectedErr = std::wstring(wsl::shared::Localization::MessageWslaSessionNotFound(name.c_str()));
        ValidateWsladiagOutput(std::format(L"shell {} --verbose", name), 1, L"", expectedErr);
>>>>>>> ea162030 (Localize Wsladiag tests)
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
        std::wstring cmd = BuildWsladiagCmd(args);
        return LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data());
    }

    // Validate that list command output shows either no sessions message or session table
    static void ValidateListOutput(const std::wstring& out)
    {
        const bool noSessions = out.find(std::wstring(wsl::shared::Localization::MessageWslaNoSessionsFound())) != std::wstring::npos;
        const auto idHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderId());
        const auto pidHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderCreatorPid());
        const auto nameHeader = std::wstring(wsl::shared::Localization::MessageWslaHeaderDisplayName());

        const bool hasTable = (out.find(idHeader) != std::wstring::npos) && (out.find(pidHeader) != std::wstring::npos) &&
                              (out.find(nameHeader) != std::wstring::npos);

        VERIFY_IS_TRUE(noSessions || hasTable);
    }

    // Validate that usage information contains expected command descriptions
    static void ValidateUsage(const std::wstring& err)
    {
<<<<<<< HEAD
        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag list") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag shell <SessionName> [--verbose]") != std::wstring::npos);
    }

    // Validate that session not found error contains the expected session name
    static void ValidateSessionNotFound(const std::wstring& err, const std::wstring& name)
    {
        VERIFY_IS_TRUE(err.find(std::format(L"Session not found: '{}'", name)) != std::wstring::npos);
=======
        const std::wstring nerr = NormalizeForCompare(err);
        const std::wstring usage = NormalizeForCompare(GetUsageText());
        VERIFY_IS_TRUE(nerr.find(usage) != std::wstring::npos);
>>>>>>> ea162030 (Localize Wsladiag tests)
    }
};
} // namespace WsladiagTests