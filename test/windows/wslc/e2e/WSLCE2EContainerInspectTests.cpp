/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container inspect command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerInspectTests)

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

    TEST_METHOD(WSLCE2E_Container_Inspect_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Inspect_MissingContainerId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Inspect_ContainerNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container inspect {}", WslcContainerName));
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Inspect_Success)
    {
        WSL2_TEST_ONLY();

        // Create a container first
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = createResult.GetStdoutOneLine();

        // Inspect by ID
        auto result = RunWslc(std::format(L"container inspect {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData = wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_ARE_EQUAL(containerId, wsl::shared::string::MultiByteToWide(inspectData[0].Id));

        // Inspect by name
        result = RunWslc(std::format(L"container inspect {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        inspectData = wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
    }

    TEST_METHOD(WSLCE2E_Container_Inspect_Multiple)
    {
        WSL2_TEST_ONLY();

        const std::wstring WslcContainerName2 = L"wslc-test-container-2";
        EnsureContainerDoesNotExist(WslcContainerName2);

        // Create two containers
        auto result1 = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result1.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId1 = result1.GetStdoutOneLine();

        auto result2 = RunWslc(std::format(L"container create --name {} {}", WslcContainerName2, DebianImage.NameAndTag()));
        result2.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId2 = result2.GetStdoutOneLine();

        // Inspect both
        auto result = RunWslc(std::format(L"container inspect {} {}", containerId1, containerId2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData = wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2u, inspectData.size());

        // Cleanup
        EnsureContainerDoesNotExist(WslcContainerName2);
    }

    TEST_METHOD(WSLCE2E_Container_Inspect_VerboseOption)
    {
        WSL2_TEST_ONLY();

        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = createResult.GetStdoutOneLine();

        auto result = RunWslc(std::format(L"container inspect --verbose {}", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout.has_value());
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
        return L"Inspects a container.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container inspect [<options>] <container-id>\r\n\r\n";
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
        options << L"The following options are available:\r\n" //
                << L"  -h,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
