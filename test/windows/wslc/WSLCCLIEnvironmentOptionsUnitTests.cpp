/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIEnvironmentOptionsUnitTests.cpp

Abstract:

    Unit tests for ApplyEnvironmentOptions. Every binding in c_envBindings is
    presence-only: defining the variable sets the option regardless of value;
    unsetting it is the only opt-out. NO_COLOR follows https://no-color.org.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Argument.h"
#include "ArgumentTypes.h"
#include "EnvironmentOptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIEnvironmentOptionsUnitTests {

class WSLCCLIEnvironmentOptionsUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIEnvironmentOptionsUnitTests)

    // Tests touch process-wide env state; setup/cleanup keep them hermetic.
    TEST_METHOD_SETUP(TestMethodSetup)
    {
        ClearBoundEnvVars();
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        ClearBoundEnvVars();
        return true;
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorEmptyValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    // NO_COLOR spec: "0" / "false" / "no" / "off" are not opt-outs.
    TEST_METHOD(ApplyEnvironmentOptions_NoColorFalsyLikeValues_StillSetFlag)
    {
        for (const auto* value : {L"0", L"false", L"FALSE", L"no", L"off"})
        {
            VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", value));

            ArgMap target;
            ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

            LogComment(std::wstring(L"NO_COLOR=") + value);
            VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
            VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());

            VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", nullptr));
        }
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorArbitraryValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L"1"));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugPresent_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L"1"));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_TRUE(target.Get<ArgType::Debug>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugEmptyValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L""));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_TRUE(target.Get<ArgType::Debug>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::Debug));
    }

    // Env-derived defaults are lowest precedence and must not overwrite.
    TEST_METHOD(ApplyEnvironmentOptions_TargetAlreadyContainsArg_LeavesItUntouched)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        target.Add<ArgType::NoColor>(false);

        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::NoColor));
        VERIFY_IS_FALSE(target.Get<ArgType::NoColor>());
    }

    // Bindings outside definedArgs are ignored even if the env var is set.
    TEST_METHOD(ApplyEnvironmentOptions_UndeclaredArg_IsIgnored)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L"1"));

        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::Debug));

        ArgMap target;
        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

private:
    static std::vector<Argument> NoColorAndDebugDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::NoColor));
        defs.push_back(Argument::Create(ArgType::Debug));
        return defs;
    }

    static void ClearBoundEnvVars()
    {
        SetEnvironmentVariableW(L"NO_COLOR", nullptr);
        SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", nullptr);
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
