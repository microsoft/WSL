/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EGlobalTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

using namespace WEX::Logging;

namespace WSLCE2ETests {

class WSLCE2EGlobalTests
{
    WSLC_TEST_CLASS(WSLCE2EGlobalTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_HelpCommand)
    {
        WSL2_TEST_ONLY();
        RunWslcAndVerify(L"--help", {.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_InvalidCommand_DisplaysErrorMessage)
    {
        WSL2_TEST_ONLY();
        RunWslcAndVerify(L"INVALID_CMD", {.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
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
        return L"WSLC is the Windows Subsystem for Linux Container CLI tool. It enables management and interaction with WSL "
               L"containers from the command line.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc  [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following commands are available:\r\n"
                 << L"  container  Container command.\r\n"
                 << L"  image      Image command.\r\n"
                 << L"  session    Session command.\r\n"
                 << L"  settings   Open the settings file in the default editor.\r\n"
                 << L"  attach     Attach to a container.\r\n"
                 << L"  build      Build an image from a Dockerfile.\r\n"
                 << L"  create     Create a container.\r\n"
                 << L"  exec       Execute a command in a running container.\r\n"
                 << L"  images     List images.\r\n"
                 << L"  inspect    Inspect a container.\r\n"
                 << L"  kill       Kill containers.\r\n"
                 << L"  list       List containers.\r\n"
                 << L"  load       Load images.\r\n"
                 << L"  logs       View container logs.\r\n"
                 << L"  pull       Pull images.\r\n"
                 << L"  remove     Remove containers.\r\n"
                 << L"  rmi        Remove images.\r\n"
                 << L"  run        Run a container.\r\n"
                 << L"  save       Save images.\r\n"
                 << L"  start      Start a container.\r\n"
                 << L"  stop       Stop containers.\r\n"
                 << L"\r\n"
                 << L"For more details on a specific command, pass it the help argument. [-h]\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -v,--version  Show version information for this tool\r\n"
                << L"  -h,--help     Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests