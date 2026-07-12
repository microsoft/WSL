/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageInspectTests.cpp

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

class WSLCE2EImageInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EImageInspectTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(BuiltExposeImage);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"image inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_MissingImageName)
    {
        auto result = RunWslc(L"image inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_ImageNotFound)
    {
        auto result = RunWslc(std::format(L"image inspect {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Image '{}' not found.\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_Success)
    {
        auto result = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));

        // Verify RootFS is populated.
        VERIFY_IS_TRUE(inspectData[0].RootFS.has_value());
        VERIFY_ARE_EQUAL(std::string{"layers"}, inspectData[0].RootFS.value().Type);
        VERIFY_IS_TRUE(inspectData[0].RootFS.value().Layers.has_value());
        VERIFY_IS_GREATER_THAN(inspectData[0].RootFS.value().Layers.value().size(), 0u);

        // Debian has no ExposedPorts or Volumes.
        VERIFY_IS_TRUE(inspectData[0].Config.has_value());
        VERIFY_IS_FALSE(inspectData[0].Config.value().ExposedPorts.has_value());
        VERIFY_IS_FALSE(inspectData[0].Config.value().Volumes.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_ConfigExtras_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-inspect-config-extras";
        auto cleanupDir = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            std::format(
                "FROM {}\n"
                "EXPOSE 8080/tcp\n"
                "EXPOSE 9090/udp\n"
                "VOLUME /data\n"
                "VOLUME /var/log/app\n"
                "STOPSIGNAL SIGTERM\n",
                wsl::shared::string::WideToMultiByte(DebianImage.NameAndTag())));

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {}", contextDir.wstring(), dockerfilePath.wstring(), BuiltExposeImage.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltExposeImage.NameAndTag());
        VERIFY_IS_TRUE(inspectData.Config.has_value());
        const auto& config = inspectData.Config.value();

        VERIFY_IS_TRUE(config.ExposedPorts.has_value());
        const auto& ports = config.ExposedPorts.value();
        VERIFY_IS_TRUE(ports.contains("8080/tcp"));
        VERIFY_IS_TRUE(ports.contains("9090/udp"));

        VERIFY_IS_TRUE(config.Volumes.has_value());
        const auto& volumes = config.Volumes.value();
        VERIFY_IS_TRUE(volumes.contains("/data"));
        VERIFY_IS_TRUE(volumes.contains("/var/log/app"));

        VERIFY_ARE_EQUAL(std::string{"SIGTERM"}, config.StopSignal);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    const TestImage BuiltExposeImage{L"wslc-e2e-inspect-config-extras", L"latest", L""};

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
        return Localization::WSLCCLI_ImageInspectLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image inspect [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image      Image name\r\n"                //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"               //
                << L"  -?,--help  Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests