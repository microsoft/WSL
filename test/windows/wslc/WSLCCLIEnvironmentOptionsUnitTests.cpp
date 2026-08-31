/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIEnvironmentOptionsUnitTests.cpp

Abstract:

    Unit tests for Environment Options.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "ArgMap.h"
#include "EnvironmentOptions.h"
#include "RootCommand.h"

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

    // Tests touch process-wide env state. ScopedEnvVariable captures any
    // pre-existing value in setup, clears it, and restores it in cleanup so
    // the suite is hermetic.
    TEST_METHOD_SETUP(TestMethodSetup)
    {
        m_noColor = std::make_unique<ScopedEnvVariable>(L"NO_COLOR");
        m_session = std::make_unique<ScopedEnvVariable>(L"WSLC_SESSION");
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        m_session.reset();
        m_noColor.reset();
        return true;
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorEmptyValue_SetsFlag)
    {
        m_noColor->Set(L"");

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());
    }

    // NO_COLOR spec: "0" / "false" / "no" / "off" are not opt-outs.
    TEST_METHOD(ApplyEnvironmentOptions_NoColorFalsyLikeValues_StillSetFlag)
    {
        for (const auto* value : {L"0", L"false", L"FALSE", L"no", L"off"})
        {
            m_noColor->Set(value);

            ArgMap target;
            ApplyEnvironmentOptions(target, NoColorDefs());

            LogComment(std::wstring(L"NO_COLOR=") + value);
            VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
            VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());

            m_noColor->Clear();
        }
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorArbitraryValue_SetsFlag)
    {
        m_noColor->Set(L"1");

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_NoColorAbsent_DoesNotSetFlag)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    TEST_METHOD(ApplyEnvironmentOptions_Session_SetsValue)
    {
        m_session->Set(L"env-session");

        RootCommand root;
        ArgMap target;
        ApplyEnvironmentOptions(target, root.GetGlobalsAndEnvArguments());

        VERIFY_IS_TRUE(target.Contains(ArgType::Session));
        VERIFY_ARE_EQUAL(std::wstring(L"env-session"), target.GetValue<ArgType::Session>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_SessionAbsent_DoesNotSetValue)
    {
        ArgMap target;
        ApplyEnvironmentOptions(target, SessionDefs());

        VERIFY_IS_FALSE(target.Contains(ArgType::Session));
    }

    TEST_METHOD(ApplyEnvironmentOptions_SessionEmptyValue_DoesNotSetValue)
    {
        m_session->Set(L"");

        RootCommand root;
        ArgMap target;
        ApplyEnvironmentOptions(target, root.GetGlobalsAndEnvArguments());

        VERIFY_IS_FALSE(target.Contains(ArgType::Session));
    }

    TEST_METHOD(ApplyEnvironmentOptions_CliSessionOverridesEnvironmentSession)
    {
        m_session->Set(L"env-session");

        RootCommand root;
        ArgMap target;
        auto envDefs = root.GetGlobalsAndEnvArguments();
        ApplyEnvironmentOptions(target, envDefs);

        auto invocation = CreateInvocationFromCommandLine(L"wslc --session cli-session container list");
        root.ParseArguments(
            invocation,
            target,
            root.GetGlobalArguments(),
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true,
            /*overridableDefaults*/ envDefs);

        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::Session));
        VERIFY_ARE_EQUAL(std::wstring(L"cli-session"), target.GetValue<ArgType::Session>());
    }

    // Env-derived defaults are lowest precedence and must not overwrite.
    TEST_METHOD(ApplyEnvironmentOptions_TargetAlreadyContainsArg_LeavesItUntouched)
    {
        m_noColor->Set(L"");

        ArgMap target;
        target.Add<ArgType::NoColor>(false);

        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::NoColor));
        VERIFY_IS_FALSE(target.GetValue<ArgType::NoColor>());
    }

    // Bindings outside definedArgs are ignored even if the env var is set.
    // Verbose isn't bound to any env var and isn't NoColor, so it stays a
    // valid "declared but unrelated" stand-in: declaring it alone must not
    // cause NO_COLOR to leak into target.
    TEST_METHOD(ApplyEnvironmentOptions_UndeclaredArg_IsIgnored)
    {
        m_noColor->Set(L"");

        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::Verbose));

        ArgMap target;
        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

private:
    std::unique_ptr<ScopedEnvVariable> m_noColor;
    std::unique_ptr<ScopedEnvVariable> m_session;

    static std::vector<Argument> NoColorDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::NoColor));
        return defs;
    }

    static std::vector<Argument> SessionDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::Session));
        return defs;
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
