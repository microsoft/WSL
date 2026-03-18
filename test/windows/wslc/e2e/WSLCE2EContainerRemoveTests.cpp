/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerRemoveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerRemoveTests
{
    WSL_TEST_CLASS(WSLCE2EContainerRemoveTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Remove_HelpCommand)
    {
        auto result = RunWslc(L"container remove --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_Remove_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Remove_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");

        // Delete the container
        result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = S_OK});

        // Verify the container is no longer listed
        VerifyContainerIsNotListed(WslcContainerName);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableCommands()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Removes containers.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container remove [<options>] [<container-id>]\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: rm\r\n\r\n";
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
        options << L"The following options are available:\r\n" //
                << L"  -f,--force      Delete containers even if they are running\r\n"
                << L"  --session       Specify the session to use\r\n"
                << L"  -h,--help       Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests