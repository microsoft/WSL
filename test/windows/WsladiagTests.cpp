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

    static std::wstring BuildWsladiagCmd(const std::wstring& args)
    {
        const auto exePath = wsl::windows::common::wslutil::GetBasePath() / L"wsladiag.exe";
        VERIFY_IS_TRUE(std::filesystem::exists(exePath));

        const auto exe = exePath.wstring();
        return args.empty() ? std::format(L"\"{}\"", exe) : std::format(L"\"{}\" {}", exe, args);
    }

    static std::tuple<std::wstring, std::wstring, int> RunWsladiag(const std::wstring& args)
    {
        auto cmd = BuildWsladiagCmd(args);
        return LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data());
    }

    TEST_METHOD(List_ShowsSessionsOrNoSessions)
    {
        auto [out, err, code] = RunWsladiag(L"list");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", err);

        const bool noSessions = (out.find(L"No WSLA sessions found.") != std::wstring::npos);

        const bool hasTable = (out.find(L"WSLA session") != std::wstring::npos) && (out.find(L"ID") != std::wstring::npos) &&
                              (out.find(L"Display Name") != std::wstring::npos);

        VERIFY_IS_TRUE(noSessions || hasTable);
    }

    TEST_METHOD(Help_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"--help");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag list") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag shell <SessionName> [--verbose]") != std::wstring::npos);
    }

    TEST_METHOD(Shell_MissingName_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"shell");
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag shell <SessionName> [--verbose]") != std::wstring::npos);
    }

    TEST_METHOD(Shell_InvalidSessionName_Verbose)
    {
        auto [out, err, code] = RunWsladiag(L"shell DefinitelyNotARealSession --verbose");
        VERIFY_ARE_NOT_EQUAL(0, code);

        VERIFY_IS_TRUE(out.find(L"[diag] shell='DefinitelyNotARealSession'") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"Session not found: 'DefinitelyNotARealSession'") != std::wstring::npos);
    }

    TEST_METHOD(UnknownCommand_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"blah");
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Unknown command: 'blah'") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
    }

    TEST_METHOD(EmptyCommand_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag list") != std::wstring::npos);
        VERIFY_IS_TRUE(err.find(L"wsladiag shell <SessionName> [--verbose]") != std::wstring::npos);
    }

    TEST_METHOD(Shell_InvalidSessionName_Silent)
    {
        auto [out, err, code] = RunWsladiag(L"shell DefinitelyNotARealSession");
        VERIFY_ARE_NOT_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Session not found: 'DefinitelyNotARealSession'") != std::wstring::npos);
    }

    TEST_METHOD(Help_ShortFlag_ShowsUsage)
    {
        auto [out, err, code] = RunWsladiag(L"-h");
        VERIFY_ARE_EQUAL(0, code);
        VERIFY_ARE_EQUAL(L"", out);

        VERIFY_IS_TRUE(err.find(L"Usage:") != std::wstring::npos);
    }
};
} // namespace WsladiagTests