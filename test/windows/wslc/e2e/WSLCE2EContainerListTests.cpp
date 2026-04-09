/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ContainerModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

using namespace wsl::windows::wslc::models;
using namespace wsl::windows::common::string;

class WSLCE2EContainerListTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerListTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Container_List_HelpCommand)
    {
        auto result = RunWslc(L"container list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_AllOption)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // Find container in list output
        result = RunWslc(L"container list --no-trunc --all");
        result.Verify({.Stderr = L"", .ExitCode = 0});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_List_NoOptions_RunningContainers)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = TruncateId(result.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Find container in list output with no options
        result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
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
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, foundContainerLine->find(L"running"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_NoOptions_ExcludesCreatedContainers)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create (but do not start) a container.
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = TruncateId(result.GetStdoutOneLine());
        VERIFY_IS_FALSE(containerId.empty());

        // Default list should only show running containers.
        result = RunWslc(L"container list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        bool isListed = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(containerId) != std::wstring::npos)
            {
                isListed = true;
                break;
            }
        }

        VERIFY_IS_FALSE(isListed);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_QuietOption_OutputsIdsOnly)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        result = RunWslc(L"container list --all --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto outputLine = result.GetStdoutOneLine();

        VERIFY_ARE_EQUAL(containerId, outputLine);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_InvalidFormatOption)
    {
        const auto result = RunWslc(L"container list --format invalid");
        result.Verify({.Stderr = L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table.\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_List_JsonFormat)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        // List containers with json format
        result = RunWslc(L"container list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        // Parse json and verify we got the expected container information back
        auto containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1U, containers.size());
        VERIFY_ARE_EQUAL(containerId, wsl::shared::string::MultiByteToWide(containers[0].Id));

        // Create another container
        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId2 = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId2.empty());

        // List containers with json format again
        result = RunWslc(L"container list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        // Parse json and verify we got both containers back
        containers = wsl::shared::FromJson<std::vector<ContainerInformation>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2U, containers.size());

        // Extract container IDs
        std::vector<std::wstring> containerIds;
        for (const auto& container : containers)
        {
            containerIds.push_back(wsl::shared::string::MultiByteToWide(container.Id));
        }

        // Verify both container IDs are in the list
        VERIFY_IS_TRUE(std::find(containerIds.begin(), containerIds.end(), containerId) != containerIds.end());
        VERIFY_IS_TRUE(std::find(containerIds.begin(), containerIds.end(), containerId2) != containerIds.end());
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
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ContainerListLongDesc() + L"\r\n\r\n";
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
                << L"  --format    " << Localization::WSLCCLI_FormatArgDescription() << L"\r\n"
                << L"  --no-trunc  Do not truncate output\r\n"
                << L"  -q,--quiet  Outputs the container IDs only\r\n"
                << L"  --session   Specify the session to use\r\n"
                << L"  -h,--help   Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests