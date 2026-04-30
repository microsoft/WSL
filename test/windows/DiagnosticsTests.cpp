/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagnosticsTests.cpp

Abstract:

    Unit tests for distribution start failure diagnostics.
    Validates CreateInstanceStepDescription mapping, error message formatting,
    and backward-compatibility of the 2-parameter error messages.

--*/

#include "precomp.h"
#include "Common.h"
#include "DiagnosticsHelpers.h"

using wsl::windows::service::diagnostics::CreateInstanceStepDescription;

namespace DiagnosticsTests {
class DiagnosticsTests
{
    WSL_TEST_CLASS(DiagnosticsTests)

    // -----------------------------------------------------------------------
    // CreateInstanceStepDescription — enum-to-string mapping
    // -----------------------------------------------------------------------

    TEST_METHOD(StepDescriptionFormatDisk)
    {
        auto desc = CreateInstanceStepDescription(LxInitCreateInstanceStepFormatDisk);
        VERIFY_IS_FALSE(desc.empty(), L"FormatDisk should produce a non-empty description");
        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"FormatDisk -> \"%s\"", desc.c_str()));
    }

    TEST_METHOD(StepDescriptionMountDisk)
    {
        auto desc = CreateInstanceStepDescription(LxInitCreateInstanceStepMountDisk);
        VERIFY_IS_FALSE(desc.empty(), L"MountDisk should produce a non-empty description");
        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"MountDisk -> \"%s\"", desc.c_str()));
    }

    TEST_METHOD(StepDescriptionLaunchSystemDistro)
    {
        auto desc = CreateInstanceStepDescription(LxInitCreateInstanceStepLaunchSystemDistro);
        VERIFY_IS_FALSE(desc.empty(), L"LaunchSystemDistro should produce a non-empty description");
        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"LaunchSystemDistro -> \"%s\"", desc.c_str()));
    }

    TEST_METHOD(StepDescriptionRunTar)
    {
        auto desc = CreateInstanceStepDescription(LxInitCreateInstanceStepRunTar);
        VERIFY_IS_FALSE(desc.empty(), L"RunTar should produce a non-empty description");
        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"RunTar -> \"%s\"", desc.c_str()));
    }

    TEST_METHOD(StepDescriptionNone)
    {
        auto desc = CreateInstanceStepDescription(LxInitCreateInstanceStepNone);
        VERIFY_IS_FALSE(desc.empty(), L"None (default) should produce a non-empty description");
        // None is not a known step, so it should fall through to the default
        auto unknownDesc = CreateInstanceStepDescription(static_cast<LX_MINI_CREATE_INSTANCE_STEP>(0xFF));
        VERIFY_ARE_EQUAL(desc, unknownDesc, L"None and an arbitrary unknown value should both hit the default case");
        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"None/Unknown -> \"%s\"", desc.c_str()));
    }

    TEST_METHOD(StepDescriptionUnknownFutureValue)
    {
        // Verify that any unrecognized enum value falls back gracefully.
        auto desc = CreateInstanceStepDescription(static_cast<LX_MINI_CREATE_INSTANCE_STEP>(999));
        VERIFY_IS_FALSE(desc.empty(), L"Unknown future value should produce a non-empty fallback description");
    }

    // LxInitCreateInstanceStepLaunchInit has the same numeric value (0x3) as
    // LxInitCreateInstanceStepLaunchSystemDistro. Verify they produce the same description.
    TEST_METHOD(StepDescriptionLaunchInitAliasMatchesLaunchSystemDistro)
    {
        auto descInit = CreateInstanceStepDescription(LxInitCreateInstanceStepLaunchInit);
        auto descSysDistro = CreateInstanceStepDescription(LxInitCreateInstanceStepLaunchSystemDistro);
        VERIFY_ARE_EQUAL(
            descInit, descSysDistro, L"LaunchInit (0x3) and LaunchSystemDistro (0x3) should map to the same description");
    }

    // -----------------------------------------------------------------------
    // All defined enum values produce distinct descriptions (except the alias)
    // -----------------------------------------------------------------------

    TEST_METHOD(StepDescriptionsAreDistinctPerStep)
    {
        auto formatDisk = CreateInstanceStepDescription(LxInitCreateInstanceStepFormatDisk);
        auto mountDisk = CreateInstanceStepDescription(LxInitCreateInstanceStepMountDisk);
        auto launchDistro = CreateInstanceStepDescription(LxInitCreateInstanceStepLaunchSystemDistro);
        auto runTar = CreateInstanceStepDescription(LxInitCreateInstanceStepRunTar);
        auto unknown = CreateInstanceStepDescription(LxInitCreateInstanceStepNone);

        VERIFY_ARE_NOT_EQUAL(formatDisk, mountDisk, L"FormatDisk != MountDisk");
        VERIFY_ARE_NOT_EQUAL(formatDisk, launchDistro, L"FormatDisk != LaunchDistro");
        VERIFY_ARE_NOT_EQUAL(formatDisk, runTar, L"FormatDisk != RunTar");
        VERIFY_ARE_NOT_EQUAL(mountDisk, launchDistro, L"MountDisk != LaunchDistro");
        VERIFY_ARE_NOT_EQUAL(mountDisk, runTar, L"MountDisk != RunTar");
        VERIFY_ARE_NOT_EQUAL(launchDistro, runTar, L"LaunchDistro != RunTar");

        // Each known step should also differ from the unknown/default description
        VERIFY_ARE_NOT_EQUAL(formatDisk, unknown, L"FormatDisk != Unknown");
        VERIFY_ARE_NOT_EQUAL(mountDisk, unknown, L"MountDisk != Unknown");
        VERIFY_ARE_NOT_EQUAL(launchDistro, unknown, L"LaunchDistro != Unknown");
        VERIFY_ARE_NOT_EQUAL(runTar, unknown, L"RunTar != Unknown");
    }

    // -----------------------------------------------------------------------
    // Error message format validation — backward compatibility
    // -----------------------------------------------------------------------

    TEST_METHOD(GenericErrorMessageContainsStepAndErrorCode)
    {
        // Verify MessageDistributionFailedToStart produces a message that
        // includes the step info and Linux error code.
        constexpr int step = static_cast<int>(LxInitCreateInstanceStepMountDisk);
        auto stepDesc = CreateInstanceStepDescription(LxInitCreateInstanceStepMountDisk);
        auto stepInfo = std::to_wstring(step) + L" (" + stepDesc + L")";
        constexpr int linuxError = 5; // EIO

        auto msg = wsl::shared::Localization::MessageDistributionFailedToStart(linuxError, stepInfo);

        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"Generic error message: \"%s\"", msg.c_str()));
        VERIFY_IS_FALSE(msg.empty(), L"Error message should not be empty");

        // The message must contain the step info and the error code as substrings
        VERIFY_IS_TRUE(msg.find(std::to_wstring(step)) != std::wstring::npos, L"Message should contain the failure step number");
        VERIFY_IS_TRUE(
            msg.find(std::to_wstring(linuxError)) != std::wstring::npos, L"Message should contain the Linux error code");
        VERIFY_IS_TRUE(msg.find(stepDesc) != std::wstring::npos, L"Message should contain the human-readable step description");
    }

    TEST_METHOD(MountDiskErrorMessageContainsRecoveryUrl)
    {
        // Verify the mount-disk-specific message includes the recovery URL.
        constexpr int step = static_cast<int>(LxInitCreateInstanceStepMountDisk);
        auto stepDesc = CreateInstanceStepDescription(LxInitCreateInstanceStepMountDisk);
        auto stepInfo = std::to_wstring(step) + L" (" + stepDesc + L")";
        constexpr int linuxError = 28; // ENOSPC

        auto msg = wsl::shared::Localization::MessageDistributionFailedToStartMountDisk(linuxError, stepInfo);

        WEX::Logging::Log::Comment(WEX::Common::String().Format(L"MountDisk error message: \"%s\"", msg.c_str()));
        VERIFY_IS_FALSE(msg.empty(), L"MountDisk error message should not be empty");

        // Must contain recovery URL for actionable guidance
        VERIFY_IS_TRUE(
            msg.find(L"https://aka.ms/wsldiskmountrecovery") != std::wstring::npos, L"MountDisk message should contain recovery URL");

        // Must still contain the error code and step info
        VERIFY_IS_TRUE(
            msg.find(std::to_wstring(step)) != std::wstring::npos, L"MountDisk message should contain the failure step number");
        VERIFY_IS_TRUE(
            msg.find(std::to_wstring(linuxError)) != std::wstring::npos, L"MountDisk message should contain the Linux error code");
    }

    TEST_METHOD(GenericAndMountDiskMessagesAreDifferent)
    {
        constexpr int step = static_cast<int>(LxInitCreateInstanceStepMountDisk);
        auto stepDesc = CreateInstanceStepDescription(LxInitCreateInstanceStepMountDisk);
        auto stepInfo = std::to_wstring(step) + L" (" + stepDesc + L")";
        constexpr int linuxError = 5;

        auto generic = wsl::shared::Localization::MessageDistributionFailedToStart(linuxError, stepInfo);
        auto mountDisk = wsl::shared::Localization::MessageDistributionFailedToStartMountDisk(linuxError, stepInfo);

        VERIFY_ARE_NOT_EQUAL(generic, mountDisk, L"The mount-disk-specific message should differ from the generic one");
    }

    // -----------------------------------------------------------------------
    // Error message backward compatibility — format has 2 parameters
    // -----------------------------------------------------------------------

    TEST_METHOD(ErrorMessageFormatIncludesTwoParameters)
    {
        // The format accepts (linuxError, stepInfo) — error code first to match
        // the original parameter order for non-en-US locale compatibility.
        const std::wstring stepInfo = L"42 (UNIQUE_STEP_SENTINEL)";
        constexpr int linuxError = 7777;

        auto msg = wsl::shared::Localization::MessageDistributionFailedToStart(linuxError, stepInfo);

        VERIFY_IS_TRUE(msg.find(L"42") != std::wstring::npos, L"Step number 42 should appear in formatted message");
        VERIFY_IS_TRUE(
            msg.find(L"UNIQUE_STEP_SENTINEL") != std::wstring::npos, L"Step description should appear in formatted message");
        VERIFY_IS_TRUE(msg.find(L"7777") != std::wstring::npos, L"Linux error code 7777 should appear in formatted message");
    }

    // -----------------------------------------------------------------------
    // Edge cases for error routing logic
    // -----------------------------------------------------------------------

    TEST_METHOD(CorruptedDiskErrorCodesAreDocumented)
    {
        // EINVAL and EUCLEAN (117) at MountDisk step should be treated as
        // disk corruption. This test documents the known error codes so that
        // if the enum or constants change, the test signals the need for review.
        VERIFY_ARE_EQUAL(22, EINVAL, L"EINVAL should be 22");

        // EUCLEAN is not in the Windows SDK; the code uses literal 117.
        constexpr int EUCLEAN_LINUX = 117;
        VERIFY_ARE_EQUAL(117, EUCLEAN_LINUX, L"EUCLEAN should be 117");
    }
};
} // namespace DiagnosticsTests
