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
using namespace wsl::shared;

class WSLCE2EContainerStopTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerStopTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_HelpCommand)
    {
        auto result = RunWslc(L"container stop --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_InvalidSignal)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_KillsRunningContainer)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Stop the container
        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_ByName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Stop by container name
        result = RunWslc(std::format(L"container stop {} -t 0", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_AlreadyStopped)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Stop the container
        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"exited");

        // Stop again - should succeed without error
        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container stop {} -t 0", WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_TargetedContainerOnly)
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

        // Stop only the first container
        result = RunWslc(std::format(L"container stop {} -t 0", firstContainerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify first exited, second still running
        VerifyContainerIsListed(firstContainerId, L"exited");
        VerifyContainerIsListed(secondContainerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_SignalByName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Stop the container using signal name
        result = RunWslc(std::format(L"container stop {} -s SIGKILL -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_InvalidSignalName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Try to stop with an invalid signal name
        result = RunWslc(std::format(L"container stop {} -s SIGINVALID -t 0", containerId));
        result.Verify({.Stderr = L"Invalid signal value: SIGINVALID is not a recognized signal name or number (Example: SIGKILL, kill, or 9).\r\n", .ExitCode = 1});

        // Verify container is still running after failed stop request
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_InvalidTimeout)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        {
            // Invalid integer
            result = RunWslc(std::format(L"container stop {} -t abc", containerId));
            result.Verify({.Stderr = L"Invalid time argument value: abc\r\n", .ExitCode = 1});

            // Should still be running after failed stop
            VerifyContainerIsListed(containerId, L"running");
        }

        {
            // Another invalid integer shape
            result = RunWslc(std::format(L"container stop {} -t 1.5", containerId));
            result.Verify({.Stderr = L"Invalid time argument value: 1.5\r\n", .ExitCode = 1});

            // Should still be running after failed stop
            VerifyContainerIsListed(containerId, L"running");
        }

        {
            // Invalid integer prefixed
            result = RunWslc(std::format(L"container stop {} -t 9abc", containerId));
            result.Verify({.Stderr = L"Invalid time argument value: 9abc\r\n", .ExitCode = 1});

            // Should still be running after failed stop
            VerifyContainerIsListed(containerId, L"running");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Stop_ValidTimeoutNegativeOne)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // -1 is a valid timeout value
        result = RunWslc(std::format(L"container stop {} -t -1", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
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
        return Localization::WSLCCLI_ContainerStopLongDesc() + L"\r\n\r\n";
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