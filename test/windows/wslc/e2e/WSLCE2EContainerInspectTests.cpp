/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wslc_schema.h>

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::shared::string;

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
        EnsureContainerDoesNotExist(TestContainerName1);
        EnsureContainerDoesNotExist(TestContainerName2);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(TestContainerName1);
        EnsureContainerDoesNotExist(TestContainerName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"container inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Inspect_MissingContainerId)
    {
        auto result = RunWslc(L"container inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Inspect_ContainerNotFound)
    {
        auto result = RunWslc(std::format(L"container inspect {}", TestContainerName1));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Container '{}' not found.\r\n", TestContainerName1), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Inspect_Success)
    {
        auto createResult = RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto result = RunWslc(std::format(L"container inspect {}", TestContainerName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_ARE_EQUAL(WideToMultiByte(TestContainerName1), inspectData[0].Name);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_InspectMultiple_Success)
    {
        // Create two containers to inspect at the same time
        auto result = RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"container create --name {} {}", TestContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Inspect both containers in the same command
        result = RunWslc(std::format(L"container inspect {} {}", TestContainerName1, TestContainerName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2u, inspectData.size());
        VERIFY_ARE_EQUAL(WideToMultiByte(TestContainerName1), inspectData[0].Name);
        VERIFY_ARE_EQUAL(WideToMultiByte(TestContainerName2), inspectData[1].Name);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Inspect_MixedFoundNotFound)
    {
        // Create one container but not the other
        auto result = RunWslc(std::format(L"container create --name {} {}", TestContainerName1, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Inspect both containers in the same command, expecting one to be found and the other to not be found
        result = RunWslc(std::format(L"container inspect {} {}", TestContainerName1, TestContainerName2));
        result.Verify({.Stderr = std::format(L"Container '{}' not found.\r\n", TestContainerName2), .ExitCode = 1});

        // Verify found container
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_ARE_EQUAL(WideToMultiByte(TestContainerName1), inspectData[0].Name);
    }

private:
    const std::wstring TestContainerName1 = L"wslc-e2e-container-inspect-1";
    const std::wstring TestContainerName2 = L"wslc-e2e-container-inspect-2";
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
        return Localization::WSLCCLI_ContainerInspectLongDesc() + L"\r\n\r\n";
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
        options << L"The following options are available:\r\n"                    //
                << L"  --session       Specify the session to use\r\n"            //
                << L"  -?,--help       Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
