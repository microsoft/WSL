/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumePruneTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC volume prune command.
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

    static constexpr auto TestVolumeName1 = L"prune-test-vol-1";
    static constexpr auto TestVolumeName2 = L"prune-test-vol-2";
    static constexpr auto TestContainerName = L"prune-vol-test-ctr";

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(TestContainerName);
        EnsureVolumeDoesNotExist(TestVolumeName1);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(TestContainerName);
        EnsureVolumeDoesNotExist(TestVolumeName1);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"volume prune --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_NoUnusedVolumes)
    {
        const auto result = RunWslc(L"volume prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_AllFlag)
    {
        // Create a named volume, then prune with --all
        RunWslc(std::format(L"volume create --name {}", TestVolumeName1)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName1);

        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Deleted:");
        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify the volume was actually removed
        VerifyVolumeIsNotListed(TestVolumeName1);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_MultipleVolumes)
    {
        // Create two named volumes, prune with --all
        RunWslc(std::format(L"volume create --name {}", TestVolumeName1)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"volume create --name {}", TestVolumeName2)).Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsListed(TestVolumeName1);
        VerifyVolumeIsListed(TestVolumeName2);

        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify both volumes were removed
        VerifyVolumeIsNotListed(TestVolumeName1);
        VerifyVolumeIsNotListed(TestVolumeName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_VolumeInUseNotPruned)
    {
        // Create a volume and a container using it
        RunWslc(std::format(L"volume create --name {}", TestVolumeName1)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"container run -d --name {} -v {}:/data {} sleep infinity",
                            TestContainerName,
                            TestVolumeName1,
                            DebianImage.NameAndTag()))
            .Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() {
            EnsureContainerDoesNotExist(TestContainerName);
            EnsureVolumeDoesNotExist(TestVolumeName1);
        });

        // Prune with --all should not remove the volume in use
        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Volume should still exist because it is mounted by a running container
        VerifyVolumeIsListed(TestVolumeName1);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Prune_IdempotentSecondPrune)
    {
        // Create a volume, prune it, then prune again
        RunWslc(std::format(L"volume create --name {}", TestVolumeName1)).Verify({.Stderr = L"", .ExitCode = 0});

        RunWslc(L"volume prune --all").Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsNotListed(TestVolumeName1);

        // Second prune should succeed with nothing to prune
        const auto result = RunWslc(L"volume prune --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");
    }

private:
    const TestImage& DebianImage = DebianTestImage();

    static void VerifyStdoutContains(const WSLCExecutionResult& result, const std::wstring& substring)
    {
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(substring) != std::wstring::npos)
            {
                return;
            }
        }

        VERIFY_FAIL(std::format(L"Expected stdout to contain '{}'", substring).c_str());
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
                << L"  -a,--all   " << Localization::WSLCCLI_VolumePruneAllArgDescription() << L"\r\n"
                << L"  --session  " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
