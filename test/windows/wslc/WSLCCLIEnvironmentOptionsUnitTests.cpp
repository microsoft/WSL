/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIEnvironmentOptionsUnitTests.cpp

Abstract:

    Unit tests for EnvironmentOptions: process-environment-sourced global
    options (e.g. WSLC_CLI_DEBUG, NO_COLOR) and their binding table.

    Not to be confused with WSLCCLIEnvVarParserUnitTests, which covers the
    --env / --env-file CLI value parsers used to build container env blocks.

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

namespace {

    // RAII: sets an env var on construction, restores prior state on destruction.
    // Tests must not leak env state because c_envBindings reads from the process.
    struct EnvVarScope
    {
        EnvVarScope(const wchar_t* name, const wchar_t* value) : m_name(name)
        {
            DWORD len = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (len > 0)
            {
                std::wstring prior(len - 1, L'\0');
                DWORD got = ::GetEnvironmentVariableW(name, prior.data(), len);
                if (got > 0 && got < len)
                {
                    m_prior = std::move(prior);
                }
            }

            ::SetEnvironmentVariableW(name, value);
        }

        ~EnvVarScope()
        {
            ::SetEnvironmentVariableW(m_name, m_prior.has_value() ? m_prior->c_str() : nullptr);
        }

        EnvVarScope(const EnvVarScope&) = delete;
        EnvVarScope& operator=(const EnvVarScope&) = delete;
        EnvVarScope(EnvVarScope&&) = delete;
        EnvVarScope& operator=(EnvVarScope&&) = delete;

    private:
        const wchar_t* m_name;
        std::optional<std::wstring> m_prior;
    };

    bool BindingsContain(const wchar_t* name, ArgType type)
    {
        for (const auto& b : c_envBindings)
        {
            if (b.Type == type && _wcsicmp(b.Name, name) == 0)
            {
                return true;
            }
        }
        return false;
    }

} // namespace

class WSLCCLIEnvironmentOptionsUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIEnvironmentOptionsUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // c_envBindings is the source of truth and is exposed from the header so
    // help rendering and tests can enumerate it. Pin the expected entries.
    TEST_METHOD(Bindings_ContainsExpectedEntries)
    {
        VERIFY_IS_TRUE(BindingsContain(L"WSLC_CLI_DEBUG", ArgType::Debug));
        VERIFY_IS_TRUE(BindingsContain(L"WSLC_CLI_NO_COLOR", ArgType::NoColor));
        VERIFY_IS_TRUE(BindingsContain(L"NO_COLOR", ArgType::NoColor));
    }

    // Flag set from env when its binding is truthy and CLI did not provide it.
    TEST_METHOD(Apply_Flag_TruthyEnv_SetsFlag)
    {
        EnvVarScope d(L"WSLC_CLI_DEBUG", L"1");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::Debug)};

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_TRUE(target.Get<ArgType::Debug>());
    }

    // Explicit opt-out values (0/false/no/off, case-insensitive) leave the flag unset.
    TEST_METHOD(Apply_Flag_FalseyEnv_DoesNotSetFlag)
    {
        for (const wchar_t* v : {L"0", L"false", L"FALSE", L"No", L"off"})
        {
            EnvVarScope d(L"WSLC_CLI_DEBUG", v);

            ArgMap target;
            std::vector<Argument> defs{Argument::Create(ArgType::Debug)};

            ApplyEnvironmentOptions(target, defs);

            VERIFY_IS_FALSE(target.Contains(ArgType::Debug), std::format(L"value '{}' should be falsey", v).c_str());
        }
    }

    // Empty string is treated as falsey (consistent with NO_COLOR semantics).
    TEST_METHOD(Apply_Flag_EmptyEnv_DoesNotSetFlag)
    {
        EnvVarScope d(L"WSLC_CLI_DEBUG", L"");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::Debug)};

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_FALSE(target.Contains(ArgType::Debug));
    }

    // OR across bindings: a single truthy binding wins regardless of others.
    TEST_METHOD(Apply_Flag_MultipleBindings_OrTruthy)
    {
        EnvVarScope a(L"NO_COLOR", L"0");
        EnvVarScope b(L"WSLC_CLI_NO_COLOR", L"1");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::NoColor)};

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    // OR is order-independent: truthy from the first binding still wins.
    TEST_METHOD(Apply_Flag_MultipleBindings_OrTruthy_Reversed)
    {
        EnvVarScope a(L"NO_COLOR", L"1");
        EnvVarScope b(L"WSLC_CLI_NO_COLOR", L"0");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::NoColor)};

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::NoColor));
        VERIFY_IS_TRUE(target.Get<ArgType::NoColor>());
    }

    // All bindings falsey: flag stays unset.
    TEST_METHOD(Apply_Flag_MultipleBindings_AllFalseyStaysUnset)
    {
        EnvVarScope a(L"NO_COLOR", L"0");
        EnvVarScope b(L"WSLC_CLI_NO_COLOR", L"false");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::NoColor)};

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    // CLI precedence: target already has the ArgType -> env is ignored entirely.
    TEST_METHOD(Apply_CliWinsOverEnv)
    {
        EnvVarScope d(L"WSLC_CLI_DEBUG", L"1");

        ArgMap target;
        // Simulate parser having already set Debug from the command line.
        target.Add(ArgType::Debug, false);

        std::vector<Argument> defs{Argument::Create(ArgType::Debug)};
        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_FALSE(target.Get<ArgType::Debug>()); // unchanged
    }

    // Bindings are skipped for ArgTypes not in definedArgs, so callers control scope.
    TEST_METHOD(Apply_OnlyDefinedArgsAreApplied)
    {
        EnvVarScope d(L"WSLC_CLI_DEBUG", L"1");
        EnvVarScope n(L"NO_COLOR", L"1");

        ArgMap target;
        std::vector<Argument> defs{Argument::Create(ArgType::Debug)}; // NoColor intentionally absent

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_TRUE(target.Contains(ArgType::Debug));
        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }

    // No env vars set -> no-op.
    TEST_METHOD(Apply_NoEnv_NoOp)
    {
        // Make sure nothing leaks in from the host shell.
        EnvVarScope d(L"WSLC_CLI_DEBUG", nullptr);
        EnvVarScope a(L"WSLC_CLI_NO_COLOR", nullptr);
        EnvVarScope b(L"NO_COLOR", nullptr);

        ArgMap target;
        std::vector<Argument> defs{
            Argument::Create(ArgType::Debug),
            Argument::Create(ArgType::NoColor),
        };

        ApplyEnvironmentOptions(target, defs);

        VERIFY_IS_FALSE(target.Contains(ArgType::Debug));
        VERIFY_IS_FALSE(target.Contains(ArgType::NoColor));
    }
};

} // namespace WSLCCLIEnvironmentOptionsUnitTests
