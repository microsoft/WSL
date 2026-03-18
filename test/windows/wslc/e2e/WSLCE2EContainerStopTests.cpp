/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerStopTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerStopTests
{
    WSL_TEST_CLASS(WSLCE2EContainerStopTests)

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

    TEST_METHOD(WSLCE2E_Container_Stop_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container stop --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Stop_InvalidSignal)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            result = RunWslc(std::format(L"container stop {} -s 0 -t 0", WslcContainerName));
            result.Verify({.Stderr = L"Invalid signal value: 0 is out of valid range (1-31).\r\n", .ExitCode = 1});
        }

        {
            result = RunWslc(std::format(L"container stop {} -s 32 -t 0", WslcContainerName));
            result.Verify({.Stderr = L"Invalid signal value: 32 is out of valid range (1-31).\r\n", .ExitCode = 1});
        }
    }

    TEST_METHOD(WSLCE2E_Container_Stop_KillsRunningContainer)
    {
        WSL2_TEST_ONLY();

        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Stop the container
        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    TEST_METHOD(WSLCE2E_Container_Stop_ByName)
    {
        WSL2_TEST_ONLY();

        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Stop by container name
        result = RunWslc(std::format(L"container stop {} -t 0", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    TEST_METHOD(WSLCE2E_Container_Stop_NotFound)
    {
        WSL2_TEST_ONLY();

        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container stop {} -t 0", WslcContainerName));
        result.Verify({.Stderr = std::format(L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", WslcContainerName), .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Stop_TargetedContainerOnly)
    {
        WSL2_TEST_ONLY();

        // Run first container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        const auto firstContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(firstContainerId.empty());

        // Run second container in background
        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        const auto secondContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(secondContainerId.empty());

        // Verify both are running
        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");

        // Stop only the first container
        result = RunWslc(std::format(L"container stop {} -t 0", firstContainerId));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

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
        return L"Stops containers.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container stop [<options>] [<container-id>]\r\n\r\n";
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
                << L"  -s,--signal     Signal to send (default: SIGTERM)\r\n"
                << L"  -t,--time       Time in seconds to wait before executing (default 5)\r\n"
                << L"  -h,--help       Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests