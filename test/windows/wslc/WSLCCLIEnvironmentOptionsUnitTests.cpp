/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIEnvironmentOptionsUnitTests.cpp

Abstract:

    Unit tests for ApplyEnvironmentOptions, which preloads ArgMap entries from
    the process environment before CLI parsing. Every entry in c_envBindings
    follows a presence-only contract: the variable being defined in the
    environment sets the option regardless of value (including empty). To
    "turn off" an env-bound option the user unsets the variable. NO_COLOR
    follows https://no-color.org; the vendor-specific WSLC_CLI_* variables
    use the same presence-only semantics for consistency.

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

    // Tests in this class manipulate process-wide environment variables, so
    // setup/cleanup wipe every env var bound in c_envBindings to keep
    // individual tests hermetic and order-independent.
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

    // NO_COLOR spec (https://no-color.org): presence of the variable disables
    // color regardless of value. The empty value must still trip the flag.
    TEST_METHOD(ApplyEnvironmentOptions_NoColorEmptyValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    // NO_COLOR spec: "0" / "false" / "no" / "off" must NOT be treated as
    // opt-outs because the value is explicitly ignored.
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

    // Any non-empty value (the common case) sets the flag.
    TEST_METHOD(ApplyEnvironmentOptions_NoColorArbitraryValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L"1"));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    // When NO_COLOR is absent and no other binding is set, the flag must not
    // be added to the map (NoColor must remain "unspecified").
    TEST_METHOD(ApplyEnvironmentOptions_NoColorAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    // WSLC_CLI_DEBUG is presence-only (same contract as NO_COLOR). A defined
    // variable with any value sets the flag.
    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugPresent_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L"1"));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_TRUE(target.Get<ArgType::Debug>());
    }

    // Presence-only contract: an empty value must still set the flag. This
    // is the behavior change that came with collapsing PresenceOnly + IsTruthy
    // into a single presence-only rule for every binding.
    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugEmptyValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L""));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_TRUE(target.Get<ArgType::Debug>());
    }

    // Absence is the only way to opt out.
    TEST_METHOD(ApplyEnvironmentOptions_WslcCliDebugAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::Debug));
    }

    // Defensive contract: a target that already contains the ArgType must
    // NOT be overwritten — env-derived defaults are lowest precedence.
    TEST_METHOD(ApplyEnvironmentOptions_TargetAlreadyContainsArg_LeavesItUntouched)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        target.Add<ArgType::NoColor>(false);

        ApplyEnvironmentOptions(target, NoColorAndDebugDefs());

        // The preexisting "false" must survive; env must not add a second
        // entry into the underlying multimap.
        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::NoColor));
        VERIFY_IS_FALSE(target.Get<ArgType::NoColor>());
    }

    // ArgTypes not present in definedArgs are ignored even if their env var
    // is set (the caller controls which defs are visible).
    TEST_METHOD(ApplyEnvironmentOptions_UndeclaredArg_IsIgnored)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", L"1"));

        // Only Debug is declared — NoColor must be left out of the map.
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::Debug));

        ArgMap target;
        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

private:
    // Returns the canonical set of defined args used by these tests.
    static std::vector<Argument> NoColorAndDebugDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::NoColor));
        defs.push_back(Argument::Create(ArgType::Debug));
        return defs;
    }

    // Drops every env var that ApplyEnvironmentOptions inspects so leftover
    // state from prior tests (or a developer's shell) cannot leak in.
    static void ClearBoundEnvVars()
    {
        SetEnvironmentVariableW(L"NO_COLOR", nullptr);
        SetEnvironmentVariableW(L"WSLC_CLI_DEBUG", nullptr);
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
