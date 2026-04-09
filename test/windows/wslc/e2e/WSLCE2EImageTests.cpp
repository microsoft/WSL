/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "Argument.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageTests
{
    WSLC_TEST_CLASS(WSLCE2EImageTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_HelpCommand)
    {
        auto result = RunWslc(L"image --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_NoSubcommand_ShowsHelp)
    {
        auto result = RunWslc(L"image");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_InvalidCommand_DisplaysErrorMessage)
    {
        auto result = RunWslc(L"image INVALID_CMD");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Unrecognized command: 'INVALID_CMD'\r\n", .ExitCode = 1});
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
        return Localization::WSLCCLI_ImageCommandLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::vector<std::pair<std::wstring_view, std::wstring>> entries = {
            {L"build", Localization::WSLCCLI_ImageBuildDesc()},
            {L"remove", Localization::WSLCCLI_ImageRemoveDesc()},
            {L"inspect", Localization::WSLCCLI_ImageInspectDesc()},
            {L"list", Localization::WSLCCLI_ImageListDesc()},
            {L"load", Localization::WSLCCLI_ImageLoadDesc()},
            {L"pull", Localization::WSLCCLI_ImagePullDesc()},
            {L"save", Localization::WSLCCLI_ImageSaveDesc()},
            {L"tag", Localization::WSLCCLI_ImageTagDesc()},
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
        commands << L"\r\n" << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L"]\r\n" << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -h,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests