/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerExportTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container export.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerExportTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerExportTests)

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
        ExportPath = wsl::windows::common::filesystem::GetTempFilename();
        DeleteFileW(ExportPath.c_str());
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        DeleteFileW(ExportPath.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Export_HelpCommand)
    {
        auto result = RunWslc(L"container export --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Export_MissingContainerId)
    {
        const auto result = RunWslc(std::format(L"container export --output \"{}\"", ExportPath.wstring()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Export_ContainerNotFound)
    {
        const auto result = RunWslc(std::format(L"container export --output \"{}\" {}", ExportPath.wstring(), InvalidContainerName));
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Export_ToFile_Success)
    {
        // Create a stopped container so it has a filesystem to export.
        const auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        const auto result = RunWslc(std::format(L"container export --output \"{}\" {}", ExportPath.wstring(), WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(ExportPath));
        VERIFY_ARE_NOT_EQUAL(0u, std::filesystem::file_size(ExportPath));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Export_ToStdout_Success)
    {
        const auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        const auto result = RunWslcAndRedirectToFile(std::format(L"container export {}", WslcContainerName), ExportPath);
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(ExportPath));
        VERIFY_ARE_NOT_EQUAL(0u, std::filesystem::file_size(ExportPath));
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container-export";
    const std::wstring InvalidContainerName = L"wslc-nonexistent-container-for-export";
    const TestImage& DebianImage = DebianTestImage();

    std::filesystem::path ExportPath{};

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
        return Localization::WSLCCLI_ContainerExportLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container export [<options>] <container-id>\r\n\r\n";
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
                << L"  -o,--output     Write to a file, instead of STDOUT\r\n"    //
                << L"  -?,--help       Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
