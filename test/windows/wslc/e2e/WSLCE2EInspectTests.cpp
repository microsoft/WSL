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
#include "TestImageRegistry.h"
#include <wslc_schema.h>
#include <JsonUtils.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EInspectTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(DebianImage.Name);
        EnsureNetworkDoesNotExist(WslcNetworkName);
        EnsureNetworkDoesNotExist(DebianImage.Name);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(DebianImage.Name);
        EnsureNetworkDoesNotExist(WslcNetworkName);
        EnsureNetworkDoesNotExist(DebianImage.Name);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"inspect --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_MissingObjectId)
    {
        auto result = RunWslc(L"inspect");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'object-id'"));
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

    WSLC_TEST_METHOD(WSLCE2E_Inspect_FormatJson_IsSingleLine)
    {
        // The whole array is emitted on one compact line.
        auto result = RunWslc(std::format(L"inspect --format json {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto document = VerifyCompactJsonOutput(result);
        VERIFY_IS_TRUE(document.is_array());
        VERIFY_ARE_EQUAL(1u, document.size());
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(DebianImage.NameAndTag()), document[0]["RepoTags"][0].get<std::string>());

        // The compact rendering must not contain the pretty-printer's indentation.
        VERIFY_IS_FALSE(result.StdoutContainsSubstring(L"\n  "));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_DefaultFormat_IsIndented)
    {
        // Without --format the array stays indented over several lines.
        auto result = RunWslc(std::format(L"inspect {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_GREATER_THAN(result.GetStdoutLines().size(), 1u);
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_InvalidFormatOption)
    {
        auto result = RunWslc(std::format(L"inspect --format invalid {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json."));

        // json is the only rendering the inspect family has; `table` belongs to the list commands.
        auto tableResult = RunWslc(std::format(L"inspect --format table {}", DebianImage.NameAndTag()));
        tableResult.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(tableResult.StderrContainsSubstring(
            L"Invalid format value: table is not a recognized format type. Supported format types are: json."));
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_FormatJson_ObjectNotFound)
    {
        // An empty array is already compact, so both formats agree on "[]".
        auto result = RunWslc(std::format(L"inspect --format json {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Object not found: {}\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
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
        VERIFY_ARE_EQUAL(std::format(L"/{}", WslcContainerName), wsl::shared::string::MultiByteToWide(inspectData[0].Name));

        // Config.Labels must be present in the emitted JSON even when empty.
        auto json = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(result.Stdout.value()));
        VERIFY_IS_TRUE(json.is_array() && !json.empty());
        VERIFY_IS_TRUE(json[0].contains("Config") && json[0]["Config"].contains("Labels"));
        VERIFY_IS_TRUE(json[0]["Config"].contains("Image"));
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(DebianImage.NameAndTag()), json[0]["Config"]["Image"].get<std::string>());
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Container_InheritsImageLabels)
    {
        auto imageCleanup =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { TestImageRegistry::Instance().Delete(LabelInheritImage); });
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-inspect-inherit-labels";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "FROM debian:latest\n"
            "LABEL com.microsoft.wsl.test.inherit-me=from-image\n"
            "CMD [\"echo\", \"ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {}", contextDir.wstring(), dockerfilePath.wstring(), LabelInheritImage.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        EnsureContainerDoesNotExist(WslcContainerName);
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, LabelInheritImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto result = RunWslc(std::format(L"inspect {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());

        const auto& configLabels = inspectData[0].Config.Labels;
        auto inheritedIt = configLabels.find("com.microsoft.wsl.test.inherit-me");
        VERIFY_IS_TRUE(inheritedIt != configLabels.end());
        VERIFY_ARE_EQUAL(std::string("from-image"), inheritedIt->second);

        VERIFY_IS_TRUE(configLabels.find("com.microsoft.wsl.container.metadata") == configLabels.end());
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
            VERIFY_ARE_EQUAL(std::format(L"/{}", DebianImage.Name), wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }

        // With --type container
        {
            auto result = RunWslc(std::format(L"inspect --type container {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectContainer>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(std::format(L"/{}", DebianImage.Name), wsl::shared::string::MultiByteToWide(inspectData[0].Name));
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

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Image_PriorityOverNetwork)
    {
        // When an image and network share the same name and no --type is specified,
        // the image should be returned (image is checked before network in InspectTasks).
        EnsureNetworkDoesNotExist(DebianImage.Name);
        auto createResult = RunWslc(std::format(L"network create --driver bridge {}", DebianImage.Name));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto deleteNetwork = wil::scope_exit([&]() { EnsureNetworkDoesNotExist(DebianImage.Name); });

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

        // With --type network
        {
            auto result = RunWslc(std::format(L"inspect --type network {}", DebianImage.Name));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::Network>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(DebianImage.Name, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Inspect_Network_PriorityOverVolume)
    {
        // When a network and volume share the same name and no --type is specified,
        // the network should be returned (network is checked before volume in InspectTasks).
        EnsureNetworkDoesNotExist(WslcNetworkName);
        EnsureVolumeDoesNotExist(WslcNetworkName);

        auto createNetworkResult = RunWslc(std::format(L"network create --driver bridge {}", WslcNetworkName));
        createNetworkResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto deleteNetwork = wil::scope_exit([&]() { EnsureNetworkDoesNotExist(WslcNetworkName); });

        auto createVolumeResult = RunWslc(std::format(L"volume create {}", WslcNetworkName));
        createVolumeResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto deleteVolume = wil::scope_exit([&]() { EnsureVolumeDoesNotExist(WslcNetworkName); });

        // No type specified — network takes priority over volume
        {
            auto result = RunWslc(std::format(L"inspect {}", WslcNetworkName));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::Network>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(WslcNetworkName, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }

        // With --type network
        {
            auto result = RunWslc(std::format(L"inspect --type network {}", WslcNetworkName));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::Network>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(WslcNetworkName, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
        }

        // With --type volume
        {
            auto result = RunWslc(std::format(L"inspect --type volume {}", WslcNetworkName));
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto inspectData =
                wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
            VERIFY_ARE_EQUAL(1u, inspectData.size());
            VERIFY_ARE_EQUAL(WslcNetworkName, wsl::shared::string::MultiByteToWide(inspectData[0].Name));
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
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(L"Invalid type value: invalid is not a recognized inspect type. Supported inspect "
                                           L"types are: image, container, network, volume."));
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
    const std::wstring WslcNetworkName = L"wslc-inspect-test-network";
    const TestImage LabelInheritImage{L"wslc-e2e-inspect-inherit-labels", L"latest", L""};
};
} // namespace WSLCE2ETests
