/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerKillTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerKillTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerKillTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_HelpCommand)
    {
        auto result = RunWslc(L"container kill --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_KillsRunningContainer)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Kill the container
        result = RunWslc(std::format(L"container kill {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_ByName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Kill by container name
        result = RunWslc(std::format(L"container kill {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container kill {}", WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_InvalidSignal)
    {
        auto result = RunWslc(std::format(L"container run --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            result = RunWslc(std::format(L"container kill {} -s 0", WslcContainerName));
            result.Verify({.Stderr = L"Invalid signal value: 0 is out of valid range (1-31).\r\n", .ExitCode = 1});
        }

        {
            result = RunWslc(std::format(L"container kill {} -s 32", WslcContainerName));
            result.Verify({.Stderr = L"Invalid signal value: 32 is out of valid range (1-31).\r\n", .ExitCode = 1});
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Kill_TargetedContainerOnly)
    {
        // Run first container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto firstContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(firstContainerId.empty());

        // Run second container in background
        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto secondContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(secondContainerId.empty());

        // Verify both are running
        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");

        // Kill only the first container
        result = RunWslc(std::format(L"container kill {}", firstContainerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify first exited, second still running
        VerifyContainerIsListed(firstContainerId, L"exited");
        VerifyContainerIsListed(secondContainerId, L"running");
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ContainerKillLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container kill [<options>] <container-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" << L"  container-id    Container ID\r\n" << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  --session       Specify the session to use\r\n"
                << L"  -s,--signal     Signal to send (default: SIGKILL)\r\n"
                << L"  -?,--help       Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests