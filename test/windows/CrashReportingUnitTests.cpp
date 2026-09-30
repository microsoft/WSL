// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "CrashReporting.h"

using wsl::windows::common::crashreporting::BuildReportParameters;
using wsl::windows::common::crashreporting::c_parameterCount;
using wsl::windows::common::crashreporting::GetExecutableName;
using wsl::windows::common::crashreporting::LinuxCrashInfo;

namespace CrashReportingUnitTests {
class CrashReportingUnitTests
{
    WSL_TEST_CLASS(CrashReportingUnitTests)

    TEST_METHOD(ExecutableNameFromCorePatternEncoding)
    {
        // core_pattern's %E specifier encodes path separators as '!'.
        VERIFY_ARE_EQUAL(L"bash", GetExecutableName("!usr!bin!bash"));
        VERIFY_ARE_EQUAL(L"dash", GetExecutableName("!usr!bin!dash"));
    }

    TEST_METHOD(ExecutableNameFromConventionalPath)
    {
        VERIFY_ARE_EQUAL(L"bash", GetExecutableName("/usr/bin/bash"));
        VERIFY_ARE_EQUAL(L"crashdemo", GetExecutableName("crashdemo"));
    }

    TEST_METHOD(ExecutableNameFallsBackWhenEmpty)
    {
        VERIFY_ARE_EQUAL(L"unknown", GetExecutableName(""));
        VERIFY_ARE_EQUAL(L"unknown", GetExecutableName("!usr!bin!"));
    }

    TEST_METHOD(ParametersMatchRegisteredSignature)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Pid = 42;
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        // Watson rejects the report outright when the parameter count or the names do not match
        // the signature registered for WSLAppCrash.
        VERIFY_ARE_EQUAL(static_cast<size_t>(7), c_parameterCount);
        VERIFY_ARE_EQUAL(c_parameterCount, parameters.size());

        VERIFY_ARE_EQUAL(L"AppName", parameters[0].first);
        VERIFY_ARE_EQUAL(L"AppVersion", parameters[1].first);
        VERIFY_ARE_EQUAL(L"ModuleName", parameters[2].first);
        VERIFY_ARE_EQUAL(L"ModuleVersion", parameters[3].first);
        VERIFY_ARE_EQUAL(L"ModuleOffset", parameters[4].first);
        VERIFY_ARE_EQUAL(L"ExceptionCode", parameters[5].first);
        VERIFY_ARE_EQUAL(L"Flags", parameters[6].first);
    }

    TEST_METHOD(ParameterValuesDescribeTheCrash)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(L"bash", parameters[0].second);
        VERIFY_ARE_EQUAL(L"bash", parameters[2].second);
        VERIFY_ARE_EQUAL(L"0000000b", parameters[5].second);
        VERIFY_ARE_EQUAL(L"0", parameters[6].second);

        // Every parameter has to carry a value, an empty one changes the signature Watson sees.
        for (const auto& parameter : parameters)
        {
            VERIFY_IS_FALSE(parameter.first.empty());
            VERIFY_IS_FALSE(parameter.second.empty());
        }
    }

    TEST_METHOD(AppNameAttributesTheCrashToTheCallingApplication)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;
        info.AppName = L"contoso.exe";

        const auto parameters = BuildReportParameters(info);

        // The report buckets against the calling application while the module still names the
        // Linux binary that faulted.
        VERIFY_ARE_EQUAL(L"contoso.exe", parameters[0].second);
        VERIFY_ARE_EQUAL(L"bash", parameters[2].second);
    }

    TEST_METHOD(AppNameFallsBackToTheCrashingExecutable)
    {
        LinuxCrashInfo info{};
        info.Executable = "!usr!bin!bash";
        info.Signal = 11;

        const auto parameters = BuildReportParameters(info);

        VERIFY_ARE_EQUAL(parameters[2].second, parameters[0].second);
    }

    TEST_METHOD(SignalIsFormattedAsExceptionCode)
    {
        LinuxCrashInfo info{};
        info.Executable = "crashdemo";

        info.Signal = 6;
        VERIFY_ARE_EQUAL(L"00000006", BuildReportParameters(info)[5].second);

        info.Signal = 8;
        VERIFY_ARE_EQUAL(L"00000008", BuildReportParameters(info)[5].second);
    }
};
} // namespace CrashReportingUnitTests
