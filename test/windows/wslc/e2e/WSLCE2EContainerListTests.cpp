/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerListTests
{
    WSL_TEST_CLASS(WSLCE2EContainerListTests)

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

    TEST_METHOD(WSLCE2E_Container_List_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_List_AllOption)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Find container in list output
        result = RunWslc(L"container list --all");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto outputLines = result.GetStdoutLines();
        std::optional<std::wstring> foundContainerLine{};
        for (const auto& line : outputLines)
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                foundContainerLine = line;
                break;
            }
        }

        // Verify we found the container in the list output
        VERIFY_IS_TRUE(foundContainerLine.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, foundContainerLine->find(L"created"));
    }

    TEST_METHOD(WSLCE2E_Container_List_QuietOption)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Verify container ID is the only thing output with quiet option
        result = RunWslc(L"container list --all --quiet");
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto outputLines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(outputLines.end(), std::find(outputLines.begin(), outputLines.end(), containerId));
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
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return L"Lists containers. By default, only running containers are shown; use --all to include all containers.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container list [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: ls ps\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -a,--all    Show all regardless of state.\r\n"
                << L"  --format    Output formatting (json or table) (Default:table)\r\n"
                << L"  -q,--quiet  Outputs the container IDs only\r\n"
                << L"  --session   Specify the session to use\r\n"
                << L"  -h,--help   Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests