/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageListTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ImageModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::wslc::models;

class WSLCE2EImageListTests
{
    WSLC_TEST_CLASS(WSLCE2EImageListTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(AlpineImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_HelpCommand)
    {
        const auto result = RunWslc(L"image list --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_DisplayLoadedImage)
    {
        const auto result = RunWslc(L"image list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(DebianImage.Name) != std::wstring::npos && line.find(DebianImage.Tag) != std::wstring::npos)
            {
                return;
            }
        }

        VERIFY_FAIL(L"Failed to find the loaded image in the output");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_QuietOption_OutputsIdsOnly)
    {
        // Get the expected image ID from JSON output.
        auto jsonResult = RunWslc(L"image list --format json");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(jsonResult.Stdout.value().c_str());

        std::string debianId;
        for (const auto& image : images)
        {
            if (image.Repository == wsl::shared::string::WideToMultiByte(DebianImage.Name) &&
                image.Tag == wsl::shared::string::WideToMultiByte(DebianImage.Tag))
            {
                debianId = image.Id;
                break;
            }
        }
        VERIFY_ARE_NOT_EQUAL(std::string{}, debianId, L"Debian image was not present in `image list --format json` output");

        const auto truncatedDebianId = wsl::shared::string::MultiByteToWide(TruncateId(debianId, true));
        const auto fullDebianIdW = wsl::shared::string::MultiByteToWide(debianId);

        // Default --quiet truncates to 12 chars.
        auto truncResult = RunWslc(L"image list --quiet");
        truncResult.Verify({.Stderr = L"", .ExitCode = 0});

        bool truncatedFound = false;
        for (const auto& line : truncResult.GetStdoutLines())
        {
            if (line == truncatedDebianId)
            {
                truncatedFound = true;
                break;
            }
        }
        VERIFY_IS_TRUE(truncatedFound, L"Truncated image ID not found in --quiet output");

        // --quiet --no-trunc shows the full id with sha256: prefix.
        auto noTruncResult = RunWslc(L"image list --quiet --no-trunc");
        noTruncResult.Verify({.Stderr = L"", .ExitCode = 0});

        bool fullFound = false;
        for (const auto& line : noTruncResult.GetStdoutLines())
        {
            if (line == fullDebianIdW)
            {
                fullFound = true;
                break;
            }
        }
        VERIFY_IS_TRUE(fullFound, L"Full image ID not found in --quiet --no-trunc output");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_InvalidFormatOption)
    {
        const auto result = RunWslc(L"image list --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_JsonFormat)
    {
        const auto result = RunWslc(L"image list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(result.Stdout.value().c_str());

        VERIFY_IS_GREATER_THAN_OR_EQUAL(images.size(), 2u);

        std::vector<std::wstring> imageNames;
        for (const auto& image : images)
        {
            auto nameAndTag = std::format(
                L"{}:{}",
                wsl::shared::string::MultiByteToWide(image.Repository.value_or("<untagged>")),
                wsl::shared::string::MultiByteToWide(image.Tag.value_or("<untagged>")));
            imageNames.push_back(nameAndTag);
        }

        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), DebianImage.NameAndTag()));
        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), AlpineImage.NameAndTag()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_TableFormat_HasExpectedColumns)
    {
        const auto result = RunWslc(L"image list");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        bool foundHeader = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.find(L"REPOSITORY") != std::wstring::npos && line.find(L"TAG") != std::wstring::npos &&
                line.find(L"IMAGE ID") != std::wstring::npos && line.find(L"CREATED") != std::wstring::npos &&
                line.find(L"SIZE") != std::wstring::npos)
            {
                foundHeader = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundHeader, L"Expected table header with REPOSITORY, TAG, IMAGE ID, CREATED, SIZE columns");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Filter_MalformedValue)
    {
        // Filter values must be of the form key=value; bare keys are rejected by the CLI.
        const auto result = RunWslc(L"image list --filter dangling");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_InvalidFilterError(L"dangling")));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Filter_InvalidKey)
    {
        // Filter keys are validated by the Docker daemon, which rejects unknown keys.
        const auto result = RunWslc(L"image list --filter color=blue");
        VERIFY_ARE_EQUAL(1, result.ExitCode);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, result.Stderr->find(L"invalid filter"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Filter_Reference)
    {
        auto listNames = [&](const std::wstring& filterArgs) {
            auto r = RunWslc(std::format(L"image list --format json {}", filterArgs));
            r.Verify({.Stderr = L"", .ExitCode = 0});
            const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(r.Stdout.value().c_str());
            std::set<std::wstring> names;
            for (const auto& image : images)
            {
                names.insert(std::format(
                    L"{}:{}",
                    wsl::shared::string::MultiByteToWide(image.Repository.value_or("<untagged>")),
                    wsl::shared::string::MultiByteToWide(image.Tag.value_or("<untagged>"))));
            }
            return names;
        };

        // reference=<name> matches only the matching image.
        {
            const auto names = listNames(std::format(L"--filter reference={}", DebianImage.Name));
            VERIFY_IS_TRUE(names.contains(DebianImage.NameAndTag()));
            VERIFY_IS_FALSE(names.contains(AlpineImage.NameAndTag()));
        }

        {
            const auto names = listNames(std::format(L"--filter reference={}", AlpineImage.Name));
            VERIFY_IS_FALSE(names.contains(DebianImage.NameAndTag()));
            VERIFY_IS_TRUE(names.contains(AlpineImage.NameAndTag()));
        }

        // Multiple --filter reference= values are OR'd: both images should be returned.
        {
            const auto names = listNames(std::format(L"--filter reference={} --filter reference={}", DebianImage.Name, AlpineImage.Name));
            VERIFY_IS_TRUE(names.contains(DebianImage.NameAndTag()));
            VERIFY_IS_TRUE(names.contains(AlpineImage.NameAndTag()));
        }

        // A reference that matches nothing returns neither image.
        {
            const auto names = listNames(L"--filter reference=wslc-no-such-image-zzz");
            VERIFY_IS_FALSE(names.contains(DebianImage.NameAndTag()));
            VERIFY_IS_FALSE(names.contains(AlpineImage.NameAndTag()));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Filter_Dangling)
    {
        // dangling=false should include normal (tagged) images.
        auto result = RunWslc(L"image list --format json --filter dangling=false");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(result.Stdout.value().c_str());
        bool foundDebian = false;
        for (const auto& image : images)
        {
            if (image.Repository == wsl::shared::string::WideToMultiByte(DebianImage.Name))
            {
                foundDebian = true;
                break;
            }
        }
        VERIFY_IS_TRUE(foundDebian, L"Expected debian image to appear in dangling=false image list");

        // dangling=true should exclude all tagged images.
        result = RunWslc(L"image list --format json --filter dangling=true");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        images = wsl::shared::FromJson<std::vector<ImageInformation>>(result.Stdout.value().c_str());
        for (const auto& image : images)
        {
            VERIFY_IS_FALSE(
                image.Repository.has_value() && image.Repository.value() != "<none>",
                L"dangling=true list should not contain tagged images");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Filter_MultipleKinds)
    {
        // Mixing different filter kinds should AND: reference=<debian> AND dangling=false
        // narrows to just debian (alpine is excluded by the reference filter).
        const auto result =
            RunWslc(std::format(L"image list --format json --filter reference={} --filter dangling=false", DebianImage.Name));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(result.Stdout.value().c_str());
        bool foundDebian = false;
        for (const auto& image : images)
        {
            const auto repo = wsl::shared::string::MultiByteToWide(image.Repository.value_or(""));
            VERIFY_ARE_NOT_EQUAL(AlpineImage.Name, repo, L"alpine should not appear when filtering by reference=debian");
            if (repo == DebianImage.Name)
            {
                foundDebian = true;
            }
        }
        VERIFY_IS_TRUE(foundDebian, L"Expected debian image when combining reference and dangling filters");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_NoTrunc_ShowsFullImageId)
    {
        // Pull the full image id from JSON output (always untruncated).
        auto jsonResult = RunWslc(L"image list --format json");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto images = wsl::shared::FromJson<std::vector<ImageInformation>>(jsonResult.Stdout.value().c_str());

        std::string fullDebianId;
        for (const auto& image : images)
        {
            if (image.Repository == wsl::shared::string::WideToMultiByte(DebianImage.Name))
            {
                fullDebianId = image.Id;
                break;
            }
        }
        VERIFY_ARE_NOT_EQUAL(std::string{}, fullDebianId, L"Debian image was not present in `image list --format json` output");

        fullDebianId = GetHashId(fullDebianId, true);
        VERIFY_IS_GREATER_THAN(fullDebianId.size(), 12u);
        const auto fullDebianIdW = wsl::shared::string::MultiByteToWide(fullDebianId);
        const auto truncatedDebianIdW = fullDebianIdW.substr(0, 12);

        // Default table truncates IMAGE ID to 12 chars.
        auto truncResult = RunWslc(L"image list");
        truncResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(truncResult.StdoutContainsSubstring(truncatedDebianIdW));
        VERIFY_IS_FALSE(truncResult.StdoutContainsSubstring(fullDebianIdW));

        // --no-trunc must show the full id.
        auto noTruncResult = RunWslc(L"image list --no-trunc");
        noTruncResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(noTruncResult.StdoutContainsSubstring(fullDebianIdW));
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
};
} // namespace WSLCE2ETests