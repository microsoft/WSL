// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include "Common.h"
#include "OptionalFeature.h"
#include "WslInstall.h"

using wsl::windows::common::optionalfeature::State;
using wsl::windows::common::optionalfeature::details::DismFeatureState;
using wsl::windows::common::optionalfeature::details::MapDismFeatureState;

class OptionalFeatureTests
{
    WSL_TEST_CLASS(OptionalFeatureTests)

    TEST_METHOD(MapDismFeatureStates)
    {
        struct TestCase
        {
            DismFeatureState DismState;
            State ExpectedState;
        };

        constexpr std::array testCases{
            TestCase{DismFeatureState::NotPresent, State::Disabled},
            TestCase{DismFeatureState::Staged, State::Disabled},
            TestCase{DismFeatureState::Removed, State::Disabled},
            TestCase{DismFeatureState::UninstallPending, State::DisablePending},
            TestCase{DismFeatureState::Installed, State::Enabled},
            TestCase{DismFeatureState::InstallPending, State::EnablePending}};

        for (const auto& testCase : testCases)
        {
            VERIFY_ARE_EQUAL(
                static_cast<unsigned int>(testCase.ExpectedState), static_cast<unsigned int>(MapDismFeatureState(testCase.DismState)));
        }
    }

    TEST_METHOD(RejectUnsupportedDismFeatureStates)
    {
        constexpr std::array states{DismFeatureState::Superseded, DismFeatureState::PartiallyInstalled};
        for (const auto state : states)
        {
            const auto result = wil::ResultFromException([&]() { std::ignore = MapDismFeatureState(state); });

            VERIFY_ARE_EQUAL(E_UNEXPECTED, result);
        }
    }

    TEST_METHOD(EvaluateVirtualMachinePlatformStates)
    {
        struct TestCase
        {
            State FeatureState;
            bool RebootRequired;
            bool EnableFeature;
        };

        constexpr std::array testCases{
            TestCase{State::Enabled, false, false},
            TestCase{State::EnablePending, true, false},
            TestCase{State::Disabled, true, true},
            TestCase{State::DisablePending, true, false}};

        for (const auto& testCase : testCases)
        {
            const auto requirements = WslInstall::EvaluateOptionalComponentRequirements(false, [&](std::wstring_view featureName) {
                VERIFY_IS_TRUE(featureName == WslInstall::c_optionalFeatureNameVmp);
                return testCase.FeatureState;
            });

            VERIFY_ARE_EQUAL(testCase.RebootRequired, requirements.RebootRequired);
            VERIFY_ARE_EQUAL(testCase.EnableFeature ? 1u : 0u, static_cast<unsigned int>(requirements.ComponentsToEnable.size()));
            if (testCase.EnableFeature)
            {
                VERIFY_ARE_EQUAL(std::wstring{WslInstall::c_optionalFeatureNameVmp}, requirements.ComponentsToEnable.front());
            }
        }
    }

    TEST_METHOD(EvaluateRequiredWslFeature)
    {
        std::vector<std::wstring> queriedFeatures;
        const auto requirements = WslInstall::EvaluateOptionalComponentRequirements(true, [&](std::wstring_view featureName) {
            queriedFeatures.emplace_back(featureName);
            return featureName == WslInstall::c_optionalFeatureNameWsl ? State::Disabled : State::Enabled;
        });

        VERIFY_IS_TRUE(requirements.RebootRequired);
        VERIFY_ARE_EQUAL(2u, static_cast<unsigned int>(queriedFeatures.size()));
        VERIFY_ARE_EQUAL(std::wstring{WslInstall::c_optionalFeatureNameWsl}, queriedFeatures[0]);
        VERIFY_ARE_EQUAL(std::wstring{WslInstall::c_optionalFeatureNameVmp}, queriedFeatures[1]);
        VERIFY_ARE_EQUAL(1u, static_cast<unsigned int>(requirements.ComponentsToEnable.size()));
        VERIFY_ARE_EQUAL(std::wstring{WslInstall::c_optionalFeatureNameWsl}, requirements.ComponentsToEnable[0]);
    }

    TEST_METHOD(PropagateFeatureQueryFailure)
    {
        const auto result = wil::ResultFromException([]() {
            std::ignore = WslInstall::EvaluateOptionalComponentRequirements(
                false, [](std::wstring_view) -> State { THROW_HR(E_ACCESSDENIED); });
        });

        VERIFY_ARE_EQUAL(E_ACCESSDENIED, result);
    }
};
