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
    WSLC_TEST_CLASS(WSLCE2EContainerRemoveTests)

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

    TEST_METHOD(WSLCE2E_Container_Remove_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container remove --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Remove_NotFound)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Remove_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");

        // Delete the container
        result = RunWslc(std::format(L"container remove {}", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the container is no longer listed
        VerifyContainerIsNotListed(WslcContainerName);
    }

    TEST_METHOD(WSLCE2E_Container_Remove_ById_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"created");

        result = RunWslc(std::format(L"container remove {}", containerId));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId);
        VerifyContainerIsNotListed(WslcContainerName);
    }

    TEST_METHOD(WSLCE2E_Container_Remove_Force_RunningContainer)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Run a container so it is in running state
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        // Removing without force should fail
        result = RunWslc(std::format(L"container remove {}", containerId));

        // TODO Add .Stderr after this issue is resolved:
        // https://github.com/microsoft/WSL/issues/14510
        result.Verify({.ExitCode = 1});

        // Container should still exist and be running
        VerifyContainerIsListed(containerId, L"running");

        // Removing with force should succeed
        result = RunWslc(std::format(L"container remove --force {}", containerId));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId);
        VerifyContainerIsNotListed(WslcContainerName);
    }

    TEST_METHOD(WSLCE2E_Container_Remove_Multiple_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId1 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId1.empty());

        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId2 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId2.empty());

        VerifyContainerIsListed(containerId1, L"created");
        VerifyContainerIsListed(containerId2, L"created");

        result = RunWslc(std::format(L"container remove {} {}", containerId1, containerId2));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsNotListed(containerId1);
        VerifyContainerIsNotListed(containerId2);
        VerifyContainerIsNotListed(WslcContainerName);
        VerifyContainerIsNotListed(WslcContainerName2);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
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
        return L"Usage: wslc container remove [<options>] <container-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: delete rm\r\n\r\n";
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