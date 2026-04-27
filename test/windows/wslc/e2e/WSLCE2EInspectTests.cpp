/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wslc_schema.h>
#include <JsonUtils.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EInspectTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(DebianImage.Name);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(DebianImage.Name);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_MissingObjectId)
    {
        auto result = RunWslc(L"inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'object-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_ObjectNotFound)
    {
        auto result = RunWslc(std::format(L"inspect {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Object not found: {}\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Image_Success)
    {
        auto result = RunWslc(std::format(L"inspect {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Image_WithTypeFlag)
    {
        auto result = RunWslc(std::format(L"inspect --type image {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Image_TypeMismatch)
    {
        auto result = RunWslc(std::format(L"inspect --type container {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Object not found: {}\r\n", DebianImage.NameAndTag()), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Container_Success)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto result = RunWslc(std::format(L"inspect {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_ARE_EQUAL(WslcContainerName, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Volume_Success)
    {
        EnsureVolumeDoesNotExist(WslcVolumeName);

        auto createResult = RunWslc(std::format(L"volume create {}", WslcVolumeName));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto deleteVolume = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(WslcVolumeName); });

        auto result = RunWslc(std::format(L"inspect {}", WslcVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_ARE_EQUAL(WslcVolumeName, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Container_PriorityOverImage)
    {
        // When a container and image share the same name and no --type is specified,
        // the container should be returned (container is checked first in InspectTasks).
        EnsureContainerDoesNotExist(DebianImage.Name);
        auto createResult = RunWslc(std::format(L"container create --name {} {}", DebianImage.Name, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        // No type specified
        {
            auto result = RunWslc(std::format(L"inspect {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(DebianImage.Name, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }

        // With --type container
        {
            auto result = RunWslc(std::format(L"inspect --type container {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(DebianImage.Name, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }

        // With --type image
        {
            auto result = RunWslc(std::format(L"inspect --type image {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
            VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
            VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Image_PriorityOverVolume)
    {
        // When an image and volume share the same name and no --type is specified,
        // the image should be returned (image is checked before volume in InspectTasks).
        EnsureVolumeDoesNotExist(DebianImage.Name);
        auto createResult = RunWslc(std::format(L"volume create {}", DebianImage.Name));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto deleteVolume = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(DebianImage.Name); });

        // No type specified
        {
            auto result = RunWslc(std::format(L"inspect {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
            VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
            VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
        }

        // With --type image
        {
            auto result = RunWslc(std::format(L"inspect --type image {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
            VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
            VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
        }

        // With --type volume
        {
            auto result = RunWslc(std::format(L"inspect --type volume {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(DebianImage.Name, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_MultipleObjects)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Inspect both a container and an image in a single call
        auto result = RunWslc(std::format(L"inspect {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // The result should be a JSON array with 2 entries
        auto array = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(result.Stdout.value()));
        VERIFY_ARE_EQUAL(2u, array.size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_MultipleObjects_PartialFailure)
    {
        // Inspect a valid image and an invalid object
        auto result = RunWslc(std::format(L"inspect {} {}", DebianImage.NameAndTag(), InvalidImage.NameAndTag()));
        result.Verify({.Stderr = std::format(L"Object not found: {}\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});

        // Stdout should still contain the valid result in a JSON array
        auto array = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(result.Stdout.value()));
        VERIFY_ARE_EQUAL(1u, array.size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_InvalidTypeValue)
    {
        auto result = RunWslc(std::format(L"inspect --type invalid {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Invalid type value: invalid is not a recognized inspect type. Supported inspect types are: image, container, volume.\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_SkipsInvalidFormatError)
    {
        // Image name cannot be upper case, but root inspect command should skip this error and continue with the inspect instead of failing
        auto result = RunWslc(L"inspect UPPER_CASE_INVALID_IMAGE");
        result.Verify({.Stdout = L"[]\r\n", .Stderr = L"Object not found: UPPER_CASE_INVALID_IMAGE\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_SkipsInvalidTypeSpecifiedArgumentError)
    {
        // Container name cannot contain a colon, but root inspect command should skip this error and continue with the inspect instead of failing
        auto result = RunWslc(std::format(L"inspect {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Object not found: {}\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
    }

private:
    const std::wstring WslcContainerName = L"wslc-inspect-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const std::wstring WslcVolumeName = L"wslc-inspect-test-volume";
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
        return Localization::WSLCCLI_InspectLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc inspect [<options>] <object-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"       //
                 << L"  object-id    Name or Id of any object type\r\n" //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                 //
                << L"  -t,--type    Type of the object to inspect\r\n"         //
                << L"  --session    Specify the session to use\r\n"            //
                << L"  -?,--help    Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
