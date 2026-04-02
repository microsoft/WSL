/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {

class WSLCE2EContainerTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_HelpCommand)
    {
        WSL2_TEST_ONLY();
        RunWslc(L"container --help").Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_InvalidCommand_DisplaysErrorMessage)
    {
        WSL2_TEST_ONLY();
        RunWslc(L"container INVALID_CMD").Verify({.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
    }

private:
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
        return L"Manage the lifecycle of WSL containers, including creating, starting, stopping, and removing them.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following sub-commands are available:\r\n"
                 << L"  attach   Attach to a container.\r\n"
                 << L"  create   Create a container.\r\n"
                 << L"  exec     Execute a command in a running container.\r\n"
                 << L"  inspect  Inspect a container.\r\n"
                 << L"  kill     Kill containers.\r\n"
                 << L"  logs     View container logs.\r\n"
                 << L"  list     List containers.\r\n"
                 << L"  remove   Remove containers.\r\n"
                 << L"  run      Run a container.\r\n"
                 << L"  start    Start a container.\r\n"
                 << L"  stop     Stop containers.\r\n"
                 << L"\r\n"
                 << L"For more details on a specific command, pass it the help argument. [-h]\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  -h,--help  Shows help about the selected command\r\n\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests