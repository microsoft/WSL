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
#include "Command.h"
#include "EnvironmentOptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIEnvironmentOptionsUnitTests {
namespace {
    struct EnvironmentScopeTestCommand final : Command
    {
        EnvironmentScopeTestCommand() : Command(L"root", L"")
        {
        }

        std::vector<Argument> GetGlobalArguments() const override
        {
            return {
                CreateGlobalArgument(ArgType::Session),
                CreateGlobalArgument(ArgType::Name),
            };
        }

        std::vector<Argument> GetArguments() const override
        {
            return {
                Argument::Create(ArgType::Hostname),
                Argument::Create(ArgType::User),
            };
        }

        std::wstring ShortDescription() const override
        {
            return L"Environment scope test command";
        }

        std::wstring LongDescription() const override
        {
            return ShortDescription();
        }

    protected:
        void ExecuteInternal(CLIExecutionContext&) const override
        {
        }
    };
} // namespace

class WSLCCLIEnvironmentOptionsUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIEnvironmentOptionsUnitTests)

    // Tests touch process-wide env state. ScopedEnvVariable captures any
    // pre-existing value in setup, clears it, and restores it in cleanup so
    // the suite is hermetic.
    TEST_METHOD_SETUP(TestMethodSetup)
    {
        m_noColor = std::make_unique<ScopedEnvVariable>(L"NO_COLOR");
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
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
        VERIFY_ARE_EQUAL(Source::Environment, target.GetSource(ArgType::NoColor));
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

    TEST_METHOD(ApplyEnvironmentOptions_RepeatedApplicationIsIdempotent)
    {
        m_noColor->Set(L"");

        ArgMap target;
        ApplyEnvironmentOptions(target, NoColorDefs());
        VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());

        ApplyEnvironmentOptions(target, NoColorDefs());
        VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());

        VERIFY_ARE_EQUAL(1u, target.Count(ArgType::NoColor));
    }

    // Env-derived defaults are lowest precedence and must not overwrite.
    TEST_METHOD(ApplyEnvironmentOptions_TargetAlreadyContainsArg_LeavesItUntouched)
    {
        m_noColor->Set(L"");

        ArgMap target;
        target.Add<ArgType::NoColor>(false, Source::CommandLine);

        ApplyEnvironmentOptions(target, NoColorDefs());

        VERIFY_ARE_EQUAL(1U, target.Count(ArgType::NoColor));
        VERIFY_IS_FALSE(target.GetValue<ArgType::NoColor>());
        VERIFY_ARE_EQUAL(Source::CommandLine, target.GetSource(ArgType::NoColor));
    }

    // Bindings outside the declared environment arguments are ignored even if the env var is set.
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

    TEST_METHOD(ApplyEnvironmentOptions_BindingWithoutEnvironmentOnlyRestriction_SetsArgument)
    {
        m_noColor->Set(L"");

        ArgMap target;
        ApplyEnvironmentOptions(target, {Argument::Create(ArgType::NoColor)});

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.GetValue<ArgType::NoColor>());
    }

    TEST_METHOD(ApplyEnvironmentOptions_CommandLineOverridesDefaultsAcrossScopes)
    {
        constexpr EnvBinding bindings[] = {
            {L"WSLC_UT_GLOBAL_SESSION", ArgType::Session},
            {L"WSLC_UT_GLOBAL_NAME", ArgType::Name},
            {L"WSLC_UT_COMMAND_HOSTNAME", ArgType::Hostname},
            {L"WSLC_UT_COMMAND_USER", ArgType::User},
        };

        ScopedEnvVariable globalSession{bindings[0].Name, L"environment-global-session"};
        ScopedEnvVariable globalName{bindings[1].Name, L"environment-global-name"};
        ScopedEnvVariable commandHostname{bindings[2].Name, L"environment-command-hostname"};
        ScopedEnvVariable commandUser{bindings[3].Name, L"environment-command-user"};

        EnvironmentScopeTestCommand command;
        ArgMap target;
        InvocationCursor invocation{
            std::vector<std::wstring>{L"--session", L"command-line-global-session", L"--hostname", L"command-line-hostname"}};

        const auto globalArguments = command.GetScopedArguments(Scope::Global);
        ApplyEnvironmentOptions(target, globalArguments, bindings);
        command.ParseArguments(
            invocation,
            target,
            command.GetScopedArguments(Scope::Global, Flags::None),
            /*optionsOnly*/ true,
            /*stopOnUnknown*/ true);
        command.ValidateArguments(target, globalArguments);

        const auto commandArguments = command.GetScopedArguments(Scope::Command);
        ApplyEnvironmentOptions(target, commandArguments, bindings);
        command.ParseArguments(invocation, target, command.GetScopedArguments(Scope::Command, Flags::None));
        command.ValidateArguments(target, commandArguments);

        VERIFY_ARE_EQUAL(std::wstring{L"command-line-global-session"}, target.GetValue<ArgType::Session>());
        VERIFY_ARE_EQUAL(std::wstring{L"environment-global-name"}, target.GetValue<ArgType::Name>());
        VERIFY_ARE_EQUAL(std::wstring{L"command-line-hostname"}, target.GetValue<ArgType::Hostname>());
        VERIFY_ARE_EQUAL(std::wstring{L"environment-command-user"}, target.GetValue<ArgType::User>());
        VERIFY_ARE_EQUAL(Source::CommandLine, target.GetSource(ArgType::Session));
        VERIFY_ARE_EQUAL(Source::Environment, target.GetSource(ArgType::Name));
        VERIFY_ARE_EQUAL(Source::CommandLine, target.GetSource(ArgType::Hostname));
        VERIFY_ARE_EQUAL(Source::Environment, target.GetSource(ArgType::User));
    }

private:
    std::unique_ptr<ScopedEnvVariable> m_noColor;

    static std::vector<Argument> NoColorDefs()
    {
        std::vector<Argument> defs;
        defs.push_back(Argument::Create(ArgType::NoColor, {.Flags = Flags::EnvironmentOnly}));
        return defs;
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
