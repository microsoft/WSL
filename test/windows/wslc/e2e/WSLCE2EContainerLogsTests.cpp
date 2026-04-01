/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerLogsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container logs command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerLogsTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerLogsTests)

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

    TEST_METHOD(WSLCE2E_Container_Logs_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container logs --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Logs_ViewLogsFromExitedContainer)
    {
        WSL2_TEST_ONLY();

        // Run a container that outputs to stdout and exits
        auto result = RunWslc(std::format(
            L"container run --name {} {} echo \"Test log output\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"Test log output\n", .Stderr = L"", .ExitCode = 0});

        // View logs from the exited container
        result = RunWslc(std::format(L"container logs {}", WslcContainerName));
        result.Verify({.Stdout = L"Test log output\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Logs_ContainerNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container logs {}", WslcContainerName));
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Logs_MissingContainerId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container logs");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Logs_FollowLogs)
    {
        WSL2_TEST_ONLY();

        // Run a container in the background
        auto result = RunWslc(std::format(
            L"container run -d --name {} {} sh -c \"echo line1; sleep 1; echo line2\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // View logs with follow flag (will timeout after container exits)
        result = RunWslc(std::format(L"container logs --follow {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        // Should contain both lines
        auto output = result.Stdout.value_or(L"");
        VERIFY_IS_TRUE(output.find(L"line1") != std::wstring::npos);
        VERIFY_IS_TRUE(output.find(L"line2") != std::wstring::npos);
    }

    TEST_METHOD(WSLCE2E_Container_Logs_VerboseOption)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --name {} {} echo verbose-log", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"verbose-log\n", .Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container logs --verbose {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stdout->find(L"verbose-log"));
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
        return L"Views container logs.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container logs [<options>] <container-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  container-id    Container ID\r\n"         //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -f,--follow    Follow log output\r\n"
                << L"  -h,--help      Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
