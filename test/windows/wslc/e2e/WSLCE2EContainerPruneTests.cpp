/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerPruneTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container prune command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerPruneTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerPruneTests)

    static constexpr auto TestContainerName1 = L"prune-test-1";
    static constexpr auto TestContainerName2 = L"prune-test-2";
    static constexpr auto TestContainerRunning = L"prune-test-running";

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(TestContainerName1);
        EnsureContainerDoesNotExist(TestContainerName2);
        EnsureContainerDoesNotExist(TestContainerRunning);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(TestContainerName1);
        EnsureContainerDoesNotExist(TestContainerName2);
        EnsureContainerDoesNotExist(TestContainerRunning);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"container prune --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_NoStoppedContainers)
    {
        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_StoppedContainer)
    {
        // Create a stopped container
        RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(TestContainerName1, L"created");

        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Deleted:");
        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify the container was actually removed
        VerifyContainerIsNotListed(TestContainerName1);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_MultipleStoppedContainers)
    {
        // Create two stopped containers
        RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"container create --name {} {}", TestContainerName2, DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(TestContainerName1, L"created");
        VerifyContainerIsListed(TestContainerName2, L"created");

        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyStdoutContains(result, L"Total reclaimed space:");

        // Verify both containers were removed
        VerifyContainerIsNotListed(TestContainerName1);
        VerifyContainerIsNotListed(TestContainerName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_RunningContainerNotPruned)
    {
        // Start a running container
        RunWslc(std::format(L"container run -d --name {} {} sleep infinity", TestContainerRunning, DebianImage.NameAndTag()))
            .Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() { EnsureContainerDoesNotExist(TestContainerRunning); });

        VerifyContainerIsListed(TestContainerRunning, L"running");

        const auto result = RunWslc(L"container prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Running container should still exist after prune
        VerifyContainerIsListed(TestContainerRunning, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Prune_IdempotentSecondPrune)
    {
        // Create and prune a container, then prune again
        RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag())).Verify({.Stderr = L"", .ExitCode = 0});

        RunWslc(L"container prune").Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsNotListed(TestContainerName1);

        // Second prune should succeed with nothing to prune
        const auto result = RunWslc(L"container prune");
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
        return Localization::WSLCCLI_ContainerPruneLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container prune [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  --session  " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
