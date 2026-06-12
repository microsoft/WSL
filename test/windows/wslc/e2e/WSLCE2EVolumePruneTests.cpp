/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumePruneTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC volume prune command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EVolumePruneTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumePruneTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        CleanUpAllTestState();
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        CleanUpAllTestState();
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        CleanUpAllTestState();
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"volume prune --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_NoVolumes)
    {
        // Prune when no volumes exist should succeed and report a reclaimed-space line.
        const auto result = RunWslc(L"volume prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"Total reclaimed space:"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_NoAllFlag_PreservesNamedVolumes)
    {
        RunWslc(std::format(L"volume create {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        auto cleanup = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(TestVolumeName); });

        const auto result = RunWslc(L"volume prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto output = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(2u, output.size());
        VERIFY_ARE_EQUAL(output[0], L"");
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output[1].find(L"Total reclaimed space:"));

        VerifyVolumeIsListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_AllFlag_RemovesNamedVolume)
    {
        RunWslc(std::format(L"volume create {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        auto cleanup = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(TestVolumeName); });

        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto output = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(3u, output.size());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output[0].find(std::format(L"Deleted: {}", TestVolumeName)));
        VERIFY_ARE_EQUAL(output[1], L"");
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output[2].find(L"Total reclaimed space:"));

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_AllFlag_RemovesMultipleVolumes)
    {
        RunWslc(std::format(L"volume create {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"volume create {}", TestVolumeName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);
        VerifyVolumeIsListed(TestVolumeName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(TestVolumeName);
            EnsureVolumeDoesNotExist(TestVolumeName2);
        });

        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName2)));

        VerifyVolumeIsNotListed(TestVolumeName);
        VerifyVolumeIsNotListed(TestVolumeName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_InUseVolume_Preserved)
    {
        RunWslc(std::format(L"volume create {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        // Start a container that holds the volume open.
        RunWslc(std::format(
                    L"container run -d --name {} -v {}:/data {} sleep infinity", WslcContainerName, TestVolumeName, DebianImage.NameAndTag()))
            .Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() {
            EnsureContainerDoesNotExist(WslcContainerName);
            EnsureVolumeDoesNotExist(TestVolumeName);
        });

        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_FALSE(
            result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)),
            L"Volume in use by a running container must not be pruned");

        VerifyVolumeIsListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_LabelFilter_PreservesNonMatchingVolume)
    {
        RunWslc(std::format(L"volume create {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        auto cleanup = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(TestVolumeName); });

        // A label filter that does not match the volume should preserve it
        const auto filteredPrune = RunWslc(L"volume prune --all --filter label=wslc.test.never=present");
        filteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(
            filteredPrune.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)),
            L"Filtered prune should not have deleted the non-matching volume");
        VerifyVolumeIsListed(TestVolumeName);

        // Subsequent unfiltered prune --all should still remove it, proving
        // the filter was the reason it survived.
        const auto unfilteredPrune = RunWslc(L"volume prune --all");
        unfilteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(unfilteredPrune.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)));
        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_LabelFilter_MatchingValueIsDeleted)
    {
        RunWslc(std::format(L"volume create --label wslc.test.prune=keep {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"volume create {}", TestVolumeName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);
        VerifyVolumeIsListed(TestVolumeName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(TestVolumeName);
            EnsureVolumeDoesNotExist(TestVolumeName2);
        });

        const auto result = RunWslc(L"volume prune --all --filter label=wslc.test.prune=keep");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)));
        VERIFY_IS_FALSE(
            result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName2)),
            L"Volume without the matching label must not be deleted");

        VerifyVolumeIsNotListed(TestVolumeName);
        VerifyVolumeIsListed(TestVolumeName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_NegatedLabelFilter_PreservesLabeledVolume)
    {
        RunWslc(std::format(L"volume create --label wslc.test.keep=yes {}", TestVolumeName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"volume create {}", TestVolumeName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);
        VerifyVolumeIsListed(TestVolumeName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureVolumeDoesNotExist(TestVolumeName);
            EnsureVolumeDoesNotExist(TestVolumeName2);
        });

        const auto result = RunWslc(L"volume prune --all --filter label!=wslc.test.keep");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName2)));
        VERIFY_IS_FALSE(
            result.StdoutContainsLine(std::format(L"Deleted: {}", TestVolumeName)),
            L"Labeled volume must be preserved when prune negates that label");

        VerifyVolumeIsListed(TestVolumeName);
        VerifyVolumeIsNotListed(TestVolumeName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_Filter_MalformedValue)
    {
        const auto result = RunWslc(L"volume prune --filter label");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = Localization::WSLCCLI_InvalidFilterError(L"label") + L"\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_Filter_InvalidKey)
    {
        const auto result = RunWslc(L"volume prune --filter color=red");
        result.Verify({.Stdout = L"", .Stderr = L"invalid filter 'color'\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const std::wstring TestVolumeName = L"wslc-e2e-volume-prune";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-prune-2";
    const std::wstring WslcContainerName = L"wslc-volume-prune-test-container";

    void CleanUpAllTestState()
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
    }

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()  //
               << GetDescription() //
               << GetUsage()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_VolumePruneLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc volume prune [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -a,--all     " << Localization::WSLCCLI_VolumePruneAllArgDescription() << L"\r\n"
                << L"  -f,--filter  " << Localization::WSLCCLI_FilterArgDescription() << L"\r\n"
                << L"  --session    " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -?,--help    " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
