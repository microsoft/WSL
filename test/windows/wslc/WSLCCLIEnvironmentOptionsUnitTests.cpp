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

    // Tests touch process-wide env state. Capture pre-existing values in setup
    // and restore them in cleanup so the suite is hermetic and doesn't clobber
    // values the test host (or CI) may have set.
    TEST_METHOD_SETUP(TestMethodSetup)
    {
        m_savedNoColor = CaptureEnv(L"NO_COLOR");
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", nullptr));
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        RestoreEnv(L"NO_COLOR", m_savedNoColor);
        return true;
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorEmptyValue_SetsFlag)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());

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
            ApplyEnvironmentOptions(target, NoColorDefs());

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
        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    // Env-derived defaults are lowest precedence and must not overwrite.
    TEST_METHOD(ApplyEnvironmentOptions_TargetAlreadyContainsArg_LeavesItUntouched)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        ArgMap target;
        target.Add<ArgType::NoColor>(false);

        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::NoColor));
        VERIFY_IS_FALSE(target.Get<ArgType::NoColor>());
    }

    // Bindings outside definedArgs are ignored even if the env var is set.
    // Verbose isn't bound to any env var and isn't NoColor, so it stays a
    // valid "declared but unrelated" stand-in: declaring it alone must not
    // cause NO_COLOR to leak into target.
    TEST_METHOD(ApplyEnvironmentOptions_UndeclaredArg_IsIgnored)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"NO_COLOR", L""));

        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::Verbose));

        ArgMap target;
        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

private:
    std::optional<std::wstring> m_savedNoColor;

    static std::vector<Argument> NoColorDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::NoColor));
        return defs;
    }

    // Snapshot a process env var. nullopt means the variable was not defined;
    // an empty string means it was defined as "".
    static std::optional<std::wstring> CaptureEnv(const wchar_t* name)
    {
        std::wstring value;
        if (FAILED(wil::GetEnvironmentVariableW(name, value)))
        {
            return std::nullopt;
        }
        return value;
    }

    static void RestoreEnv(const wchar_t* name, const std::optional<std::wstring>& saved)
    {
        VERIFY_IS_TRUE(SetEnvironmentVariableW(name, saved.has_value() ? saved->c_str() : nullptr));
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
