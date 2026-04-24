/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerLogsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

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

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_HelpCommand)
    {
        auto result = RunWslc(L"container logs --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_MissingContainerId)
    {
        auto result = RunWslc(L"container logs");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Success)
    {
        auto result = RunWslc(
            std::format(L"container run --name {} {} sh -c \"echo hello && echo world\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container logs {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto lines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), L"hello"));
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), L"world"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_TailOption)
    {
        auto result = RunWslc(std::format(
            L"container run --name {} {} sh -c \"echo line1 && echo line2 && echo line3\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container logs --tail 1 {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto lines = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(1u, lines.size());
        VERIFY_ARE_EQUAL(L"line3", lines[0]);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_TimestampsOption)
    {
        auto result = RunWslc(
            std::format(L"container run --name {} {} sh -c \"echo hello\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container logs --timestamps {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_IS_TRUE(lines.size() >= 1u);
        // Timestamps are prepended in ISO 8601 / RFC 3339 format, verify by checking for 'T' separator
        VERIFY_IS_TRUE(lines[0].find(L"T") != std::wstring::npos);
        VERIFY_IS_TRUE(lines[0].find(L"hello") != std::wstring::npos);
    }

private:
    const std::wstring WslcContainerName = L"wslc-e2e-container-logs";
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
        return Localization::WSLCCLI_ContainerLogsLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container logs [<options>] <container-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"  //
                 << L"  container-id    Container name or id\r\n"  //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                                          //
                << L"  --session       Specify the session to use\r\n"                                  //
                << L"  -f,--follow     Follow log output\r\n"                                           //
                << L"  -n,--tail       Number of lines to show from the end of the logs\r\n"            //
                << L"  --since         Show logs since timestamp (e.g. unix timestamp)\r\n"             //
                << L"  --until         Show logs before timestamp (e.g. unix timestamp)\r\n"            //
                << L"  --timestamps    Show timestamps in log output\r\n"                               //
                << L"  -?,--help       Shows help about the selected command\r\n"                       //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
