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
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::wslc::models;

class WSLCE2EImageListTests
{
    WSLC_TEST_CLASS(WSLCE2EImageListTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        TestImageRegistry::Instance().EnsureLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
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
        // Get the expected image ID from JSON output. --no-trunc is required because json output
        // truncates the id by default.
        auto jsonResult = RunWslc(L"image list --format json --no-trunc");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(jsonResult);

        std::string debianId;
        for (const auto& image : images)
        {
            if (image.Repository == wsl::shared::string::WideToMultiByte(DebianImage.Name) &&
                image.Tag == wsl::shared::string::WideToMultiByte(DebianImage.Tag))
            {
                debianId = image.ID;
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

        const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(result);

        VERIFY_IS_GREATER_THAN_OR_EQUAL(images.size(), 2u);

        std::vector<std::wstring> imageNames;
        for (const auto& image : images)
        {
            auto nameAndTag = std::format(
                L"{}:{}", wsl::shared::string::MultiByteToWide(image.Repository), wsl::shared::string::MultiByteToWide(image.Tag));
            imageNames.push_back(nameAndTag);
        }

        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), DebianImage.NameAndTag()));
        VERIFY_ARE_NOT_EQUAL(imageNames.end(), std::find(imageNames.begin(), imageNames.end(), AlpineImage.NameAndTag()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_JsonFormat_MatchesDockerShape)
    {
        const std::set<std::string> expectedKeys = {
            "Containers", "CreatedAt", "CreatedSince", "Digest", "ID", "Repository", "SharedSize", "Size", "Tag", "UniqueSize"};

        const auto result = RunWslc(L"image list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto entries = ParseNdjsonOutput(result);
        VERIFY_IS_GREATER_THAN_OR_EQUAL(entries.size(), 2u);

        for (const auto& entry : entries)
        {
            std::set<std::string> keys;
            for (const auto& [key, value] : entry.items())
            {
                keys.insert(key);
                VERIFY_IS_TRUE(value.is_string(), wsl::shared::string::MultiByteToWide(std::format("'{}' must be a string", key)).c_str());
            }

            VERIFY_ARE_EQUAL(expectedKeys, keys, L"json output must contain exactly docker's image fields");

            VERIFY_ARE_NOT_EQUAL(std::string{}, entry["Repository"].get<std::string>());
            VERIFY_ARE_NOT_EQUAL(std::string{}, entry["Tag"].get<std::string>());
            VERIFY_ARE_EQUAL(std::string{c_none}, entry["Digest"].get<std::string>());

            const auto containers = entry["Containers"].get<std::string>();
            VERIFY_IS_FALSE(containers.empty());
            VERIFY_IS_TRUE(
                std::ranges::all_of(containers, [](char value) { return std::isdigit(static_cast<unsigned char>(value)) != 0; }),
                L"'Containers' must be a container count");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_JsonFormat_ReportsContainerCount)
    {
        // Every container created from an image is counted, including containers that were never
        // started, and the count is reported against every tag of that image.
        constexpr auto containerName = L"wslc-image-list-container-count";
        EnsureContainerDoesNotExist(containerName);

        auto containerCount = [](const TestImage& image) {
            const auto result = RunWslc(L"image list --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});

            for (const auto& entry : ParseNdjsonOutputAs<ImageOutputInformation>(result))
            {
                if (entry.Repository == wsl::shared::string::WideToMultiByte(image.Name) &&
                    entry.Tag == wsl::shared::string::WideToMultiByte(image.Tag))
                {
                    return std::stoi(entry.Containers);
                }
            }

            VERIFY_FAIL(std::format(L"Image '{}' not found in image list output", image.NameAndTag()).c_str());
            return -1;
        };

        const auto alpineBaseline = containerCount(AlpineImage);
        const auto debianBaseline = containerCount(DebianImage);

        auto createResult = RunWslc(std::format(L"container create --name {} {}", containerName, AlpineImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto cleanup = wil::scope_exit([&]() { EnsureContainerDoesNotExist(containerName); });

        VERIFY_ARE_EQUAL(alpineBaseline + 1, containerCount(AlpineImage), L"a created container must be counted");
        VERIFY_ARE_EQUAL(debianBaseline, containerCount(DebianImage), L"only the image the container was created from is counted");

        cleanup.reset();

        VERIFY_ARE_EQUAL(alpineBaseline, containerCount(AlpineImage), L"a removed container must no longer be counted");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_JsonFormat_TruncatesIdByDefault)
    {
        // The id is truncated to 12 hex characters unless --no-trunc is passed, in which case it
        // keeps the sha256: prefix.
        auto truncResult = RunWslc(L"image list --format json");
        truncResult.Verify({.Stderr = L"", .ExitCode = 0});

        for (const auto& image : ParseNdjsonOutputAs<ImageOutputInformation>(truncResult))
        {
            VERIFY_ARE_EQUAL(12u, image.ID.size(), L"json ids must be truncated to 12 characters by default");
            VERIFY_IS_FALSE(image.ID.starts_with("sha256:"));
        }

        auto noTruncResult = RunWslc(L"image list --format json --no-trunc");
        noTruncResult.Verify({.Stderr = L"", .ExitCode = 0});

        for (const auto& image : ParseNdjsonOutputAs<ImageOutputInformation>(noTruncResult))
        {
            VERIFY_IS_TRUE(image.ID.starts_with("sha256:"), L"--no-trunc ids must keep the algorithm prefix");
            VERIFY_IS_GREATER_THAN(image.ID.size(), 12u);
        }
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

    WSLC_TEST_METHOD(WSLCE2E_Image_List_TableFormat_MatchesJsonValues)
    {
        // The table and json output must report the same values, formatted the way docker formats
        // them: SI sizes ("120MB", not "119.86 MB") and "<none>" for missing repository/tag data.
        const auto jsonResult = RunWslc(L"image list --format json");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});

        const auto tableResult = RunWslc(L"image list");
        tableResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto tableLines = tableResult.GetStdoutLines();

        const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(jsonResult);
        VERIFY_IS_GREATER_THAN_OR_EQUAL(images.size(), 2u);

        for (const auto& image : images)
        {
            const auto row = std::format(
                L"{} {} {} {}",
                wsl::shared::string::MultiByteToWide(image.Repository),
                wsl::shared::string::MultiByteToWide(image.Tag),
                wsl::shared::string::MultiByteToWide(image.ID),
                wsl::shared::string::MultiByteToWide(image.Size));

            const bool found = std::ranges::any_of(tableLines, [&](const auto& line) {
                // Columns are padded, so match on the individual values rather than the row text.
                return line.find(wsl::shared::string::MultiByteToWide(image.ID)) != std::wstring::npos &&
                       line.find(wsl::shared::string::MultiByteToWide(image.Repository)) != std::wstring::npos &&
                       line.find(wsl::shared::string::MultiByteToWide(image.Tag)) != std::wstring::npos &&
                       line.find(wsl::shared::string::MultiByteToWide(image.Size)) != std::wstring::npos &&
                       line.find(wsl::shared::string::MultiByteToWide(image.CreatedSince)) != std::wstring::npos;
            });

            VERIFY_IS_TRUE(found, std::format(L"Table output has no row matching json values: {}", row).c_str());
        }

        // An untagged image is never labeled "<untagged>" in either format.
        for (const auto& line : tableLines)
        {
            VERIFY_ARE_EQUAL(std::wstring::npos, line.find(L"<untagged>"), L"table must use the '<none>' placeholder");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_TableFormat_NoTruncKeepsAlgorithmPrefix)
    {
        // The --no-trunc table keeps the "sha256:" prefix on the image id.
        const auto result = RunWslc(L"image list --no-trunc");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_IS_GREATER_THAN_OR_EQUAL(lines.size(), 2u);

        for (size_t i = 1; i < lines.size(); i++)
        {
            if (!lines[i].empty())
            {
                VERIFY_ARE_NOT_EQUAL(
                    std::wstring::npos, lines[i].find(L"sha256:"), L"--no-trunc table ids must keep the algorithm prefix");
            }
        }
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
            const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(r);
            std::set<std::wstring> names;
            for (const auto& image : images)
            {
                names.insert(std::format(
                    L"{}:{}", wsl::shared::string::MultiByteToWide(image.Repository), wsl::shared::string::MultiByteToWide(image.Tag)));
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

        auto images = ParseNdjsonOutputAs<ImageOutputInformation>(result);
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

        images = ParseNdjsonOutputAs<ImageOutputInformation>(result);
        for (const auto& image : images)
        {
            VERIFY_ARE_EQUAL(
                std::string{wsl::windows::wslc::models::c_none},
                image.Repository,
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

        const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(result);
        bool foundDebian = false;
        for (const auto& image : images)
        {
            const auto repo = wsl::shared::string::MultiByteToWide(image.Repository);
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
        // Pull the full image id from JSON output. --no-trunc is required because json output
        // truncates the id by default.
        auto jsonResult = RunWslc(L"image list --format json --no-trunc");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});
        const auto images = ParseNdjsonOutputAs<ImageOutputInformation>(jsonResult);

        std::string fullDebianId;
        for (const auto& image : images)
        {
            if (image.Repository == wsl::shared::string::WideToMultiByte(DebianImage.Name))
            {
                fullDebianId = image.ID;
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

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_AddsColumnBetweenTagAndImageId)
    {
        const auto result = RunWslc(L"image list --digests");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto lines = result.GetStdoutLines();
        VERIFY_IS_FALSE(lines.empty());

        // The `--digests` table: REPOSITORY TAG DIGEST IMAGE ID CREATED SIZE.
        const auto& header = lines[0];
        const auto tag = header.find(L"TAG");
        const auto digest = header.find(L"DIGEST");
        const auto imageId = header.find(L"IMAGE ID");

        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, digest, L"DIGEST column missing from `image list --digests`");
        VERIFY_IS_TRUE(tag < digest, L"DIGEST must follow TAG");
        VERIFY_IS_TRUE(digest < imageId, L"DIGEST must precede IMAGE ID");

        // The default listing must not gain the column.
        const auto defaultResult = RunWslc(L"image list");
        defaultResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(defaultResult.GetStdoutLines()[0].find(L"DIGEST") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_ReportsDigestOnlyWhenRequested)
    {
        const auto digestFor = [](const std::wstring& command, const std::string& repository) {
            auto result = RunWslc(command);
            result.Verify({.Stderr = L"", .ExitCode = 0});

            for (const auto& image : ParseNdjsonOutputAs<ImageOutputInformation>(result))
            {
                if (image.Repository == repository)
                {
                    return image.Digest;
                }
            }

            return std::string{};
        };

        const auto debian = wsl::shared::string::WideToMultiByte(DebianImage.Name);

        // The digest is only reported when --digests is passed. json output does
        // not imply it, because reporting is gated on the flag or an explicit {{.Digest}} in the format.
        VERIFY_ARE_EQUAL(std::string{c_none}, digestFor(L"image list --format json", debian));

        // The test images are loaded from a tarball, so they carry no repo digest and
        // report "<none>" even when digests are requested. Anything reported must be a bare digest.
        const auto digest = digestFor(L"image list --digests --format json", debian);
        VERIFY_ARE_NOT_EQUAL(std::string{}, digest, L"Debian image was missing from `image list --digests --format json`");

        if (digest != c_none)
        {
            VERIFY_IS_TRUE(digest.starts_with("sha256:"), L"Digest should be reported as 'sha256:...'");
            VERIFY_IS_TRUE(digest.find('@') == std::string::npos, L"Digest should not retain the 'repo@' prefix");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_TableMatchesJson)
    {
        auto jsonResult = RunWslc(L"image list --digests --format json");
        jsonResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto tableResult = RunWslc(L"image list --digests");
        tableResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Every digest reported by json output must appear in the table's DIGEST column, so the two
        // renderings cannot drift.
        for (const auto& image : ParseNdjsonOutputAs<ImageOutputInformation>(jsonResult))
        {
            const auto digest = wsl::shared::string::MultiByteToWide(image.Digest);
            VERIFY_IS_TRUE(
                tableResult.StdoutContainsSubstring(digest),
                WEX::Common::String().Format(L"'%ls' was missing from the table DIGEST column", digest.c_str()));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_RepeatsIdOncePerDigest)
    {
        auto result = RunWslc(L"image list --digests --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto digestRows = ParseNdjsonOutputAs<ImageOutputInformation>(result);

        // A tag carrying several digests is emitted once per digest, so
        // rows stay unique per repository, tag and digest while the image ID repeats across them.
        std::map<std::pair<std::string, std::string>, std::string> idByRepoTag;
        std::set<std::tuple<std::string, std::string, std::string>> seen;

        for (const auto& image : digestRows)
        {
            const auto it = idByRepoTag.emplace(std::make_pair(image.Repository, image.Tag), image.ID).first;
            VERIFY_ARE_EQUAL(it->second, image.ID, L"Rows sharing a repository and tag must report the same image ID");

            VERIFY_IS_TRUE(
                seen.emplace(image.Repository, image.Tag, image.Digest).second,
                L"A repository, tag and digest triple must not be listed twice");

            // A digest is always qualified by the repository it was pulled from, so a row that
            // reports one names that repository instead of falling back to the placeholder.
            if (image.Digest != c_none)
            {
                VERIFY_ARE_NOT_EQUAL(std::string{c_none}, image.Repository, L"A row reporting a digest must name its repository");
            }
        }

        // Expanding per digest can only ever add rows relative to the default listing.
        auto defaultResult = RunWslc(L"image list --format json");
        defaultResult.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(digestRows.size() >= ParseNdjsonOutputAs<ImageOutputInformation>(defaultResult).size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_QuietStillOutputsIdsOnly)
    {
        const auto result = RunWslc(L"image list --digests --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // --quiet wins, so no digest or header is emitted.
        VERIFY_IS_FALSE(result.StdoutContainsSubstring(L"DIGEST"));
        VERIFY_IS_FALSE(result.StdoutContainsSubstring(L"sha256:"));

        for (const auto& line : result.GetStdoutLines())
        {
            VERIFY_ARE_EQUAL(12u, line.size(), L"--quiet should emit truncated image ids only");
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_List_Digests_ListedInHelp)
    {
        const auto result = RunWslc(L"image list --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"--digests"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_DigestsArgDescription()));
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
};
} // namespace WSLCE2ETests
