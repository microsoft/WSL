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

class WSLCE2EContainerKillTests
{
    WSL_TEST_CLASS(WSLCE2EContainerKillTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Kill_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container kill --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_Kill_KillsRunningContainer)
    {
        WSL2_TEST_ONLY();

        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container is running
        VerifyContainerIsListed(containerId, L"running");

        // Kill the container
        result = RunWslc(std::format(L"container kill {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify the container is no longer running
        VerifyContainerIsListed(containerId, L"exited");
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
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
        return L"Kills containers.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container kill [<options>] [<container-id>]\r\n\r\n";
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
                << L"  -h,--help       Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests