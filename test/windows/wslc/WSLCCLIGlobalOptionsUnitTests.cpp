/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIGlobalOptionsUnitTests.cpp

Abstract:

    This file contains unit tests for the GlobalOptions struct.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "GlobalOptions.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLI_GlobalOptionsUnitTests {

class WSLCCLI_GlobalOptionsUnitTests
{
    WSLC_TEST_CLASS(WSLCCLI_GlobalOptionsUnitTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", nullptr);
        SetEnvironmentVariableW(L"NO_COLOR", nullptr);
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", nullptr);
        SetEnvironmentVariableW(L"NO_COLOR", nullptr);
        return true;
    }

    TEST_METHOD(Resolve_NoArgsNoEnv_AllFieldsAreFalse)
    {
        const wchar_t* argv[] = {L"wslc.exe"};
        auto opts = GlobalOptions::Resolve(1, argv);
        VERIFY_IS_FALSE(opts.Debug);
        VERIFY_IS_FALSE(opts.NoColor);
    }

    TEST_METHOD(Resolve_DebugArg_SetsDebugTrue)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug"};
        auto opts = GlobalOptions::Resolve(2, argv);
        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_FALSE(opts.NoColor);
    }

    TEST_METHOD(Resolve_NoColorArg_SetsNoColorTrue)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--no-color"};
        auto opts = GlobalOptions::Resolve(2, argv);
        VERIFY_IS_FALSE(opts.Debug);
        VERIFY_IS_TRUE(opts.NoColor);
    }

    TEST_METHOD(Resolve_BothArgs_SetsBothTrue)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"--no-color"};
        auto opts = GlobalOptions::Resolve(3, argv);
        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_TRUE(opts.NoColor);
    }

    TEST_METHOD(Resolve_BothArgsReversed_SetsBothTrue)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--no-color", L"--debug"};
        auto opts = GlobalOptions::Resolve(3, argv);

        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_TRUE(opts.NoColor);
    }

    TEST_METHOD(Resolve_UnrecognizedArgFirst_NoFieldsSet)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"container", L"--debug"};
        auto opts = GlobalOptions::Resolve(3, argv);

        // "--debug" appears after an unrecognised token — must not be picked up.
        VERIFY_IS_FALSE(opts.Debug);
        VERIFY_IS_FALSE(opts.NoColor);
    }

    TEST_METHOD(Resolve_DebugThenUnrecognized_OnlyDebugSet)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"container", L"--no-color"};
        auto opts = GlobalOptions::Resolve(4, argv);

        // "--debug" is in the leading prefix; "--no-color" comes after "container" and must be ignored.
        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_FALSE(opts.NoColor);
    }

    TEST_METHOD(Resolve_WslcDebugEnvVar_SetsDebugTrue)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", L"1");
        const wchar_t* argv[] = {L"wslc.exe"};
        auto opts = GlobalOptions::Resolve(1, argv);
        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_FALSE(opts.NoColor);
    }

    TEST_METHOD(Resolve_NoColorEnvVar_SetsNoColorTrue)
    {
        SetEnvironmentVariableW(L"NO_COLOR", L"1");
        const wchar_t* argv[] = {L"wslc.exe"};
        auto opts = GlobalOptions::Resolve(1, argv);
        VERIFY_IS_FALSE(opts.Debug);
        VERIFY_IS_TRUE(opts.NoColor);
    }

    TEST_METHOD(Resolve_BothEnvVars_SetsBothTrue)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", L"1");
        SetEnvironmentVariableW(L"NO_COLOR", L"1");
        const wchar_t* argv[] = {L"wslc.exe"};
        auto opts = GlobalOptions::Resolve(1, argv);
        VERIFY_IS_TRUE(opts.Debug);
        VERIFY_IS_TRUE(opts.NoColor);
    }

    // Env vars apply regardless of whether the arg appears in argv.
    TEST_METHOD(Resolve_EnvVarAppliesEvenWhenArgAfterNonGlobal)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", L"1");

        // "--debug" is past a non-global arg, so the argv scan won't set it —
        // but the env var pass still should.
        const wchar_t* argv[] = {L"wslc.exe", L"container", L"--debug"};
        auto opts = GlobalOptions::Resolve(3, argv);
        VERIFY_IS_TRUE(opts.Debug);
    }

    // CLI arg and env var for the same field both being present is fine.
    TEST_METHOD(Resolve_ArgAndEnvVarTogether_FieldIsTrue)
    {
        SetEnvironmentVariableW(L"WSLC_DEBUG", L"1");
        const wchar_t* argv[] = {L"wslc.exe", L"--debug"};
        auto opts = GlobalOptions::Resolve(2, argv);
        VERIFY_IS_TRUE(opts.Debug);
    }

    TEST_METHOD(StripFromArgv_NoGlobalOptions_ReturnsAllArgs)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"container", L"list"};
        auto args = GlobalOptions::StripFromArgv(3, argv);
        VERIFY_ARE_EQUAL(2u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"container"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"list"), args[1]);
    }

    TEST_METHOD(StripFromArgv_OnlyGlobalOptions_ReturnsEmpty)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"--no-color"};
        auto args = GlobalOptions::StripFromArgv(3, argv);
        VERIFY_ARE_EQUAL(0u, args.size());
    }

    TEST_METHOD(StripFromArgv_DebugBeforeCommand_StripsDebug)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"container", L"list"};
        auto args = GlobalOptions::StripFromArgv(4, argv);
        VERIFY_ARE_EQUAL(2u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"container"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"list"), args[1]);
    }

    TEST_METHOD(StripFromArgv_NoColorBeforeCommand_StripsNoColor)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--no-color", L"run", L"ubuntu"};
        auto args = GlobalOptions::StripFromArgv(4, argv);
        VERIFY_ARE_EQUAL(2u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"run"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"ubuntu"), args[1]);
    }

    TEST_METHOD(StripFromArgv_BothGlobalOptionsThenCommand_StripsBoth)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"--no-color", L"image", L"list"};
        auto args = GlobalOptions::StripFromArgv(5, argv);
        VERIFY_ARE_EQUAL(2u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"image"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"list"), args[1]);
    }

    // A global option that appears AFTER the first non-global arg must NOT be stripped.
    TEST_METHOD(StripFromArgv_GlobalOptionAfterCommand_IsNotStripped)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"container", L"--debug", L"list"};
        auto args = GlobalOptions::StripFromArgv(4, argv);
        VERIFY_ARE_EQUAL(3u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"container"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"--debug"), args[1]);
        VERIFY_ARE_EQUAL(std::wstring(L"list"), args[2]);
    }

    TEST_METHOD(StripFromArgv_EmptyArgv_ReturnsEmpty)
    {
        const wchar_t* argv[] = {L"wslc.exe"};
        auto args = GlobalOptions::StripFromArgv(1, argv);
        VERIFY_ARE_EQUAL(0u, args.size());
    }

    TEST_METHOD(StripFromArgv_PreservesArgOrder)
    {
        const wchar_t* argv[] = {L"wslc.exe", L"--debug", L"run", L"-it", L"ubuntu", L"bash"};
        auto args = GlobalOptions::StripFromArgv(6, argv);
        VERIFY_ARE_EQUAL(4u, args.size());
        VERIFY_ARE_EQUAL(std::wstring(L"run"), args[0]);
        VERIFY_ARE_EQUAL(std::wstring(L"-it"), args[1]);
        VERIFY_ARE_EQUAL(std::wstring(L"ubuntu"), args[2]);
        VERIFY_ARE_EQUAL(std::wstring(L"bash"), args[3]);
    }
};

} // namespace WSLCCLI_GlobalOptionsUnitTests
