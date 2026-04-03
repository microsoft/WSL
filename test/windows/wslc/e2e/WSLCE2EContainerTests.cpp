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
#include "Argument.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

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
        return Localization::WSLCCLI_ContainerCommandLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::vector<std::pair<std::wstring_view, std::wstring>> entries = {
            {L"attach", Localization::WSLCCLI_ContainerAttachDesc()},
            {L"create", Localization::WSLCCLI_ContainerCreateDesc()},
            {L"exec", Localization::WSLCCLI_ContainerExecDesc()},
            {L"inspect", Localization::WSLCCLI_ContainerInspectDesc()},
            {L"kill", Localization::WSLCCLI_ContainerKillDesc()},
            {L"logs", Localization::WSLCCLI_ContainerLogsDesc()},
            {L"list", Localization::WSLCCLI_ContainerListDesc()},
            {L"remove", Localization::WSLCCLI_ContainerRemoveDesc()},
            {L"run", Localization::WSLCCLI_ContainerRunDesc()},
            {L"start", Localization::WSLCCLI_ContainerStartDesc()},
            {L"stop", Localization::WSLCCLI_ContainerStopDesc()},
        };

        size_t maxLen = 0;
        for (const auto& [name, _] : entries)
        {
            maxLen = (std::max)(maxLen, name.size());
        }

        std::wstringstream commands;
        commands << Localization::WSLCCLI_AvailableSubcommands() << L"\r\n";
        for (const auto& [name, desc] : entries)
        {
            commands << L"  " << name << std::wstring(maxLen - name.size() + 2, L' ') << desc << L"\r\n";
        }
        commands << L"\r\n" << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L"]\r\n\r\n";
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