/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.cpp

Abstract:

    This file contains helper functions for end-to-end tests of WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <JsonUtils.h>

extern std::wstring g_testDataPath;

namespace WSLCE2ETests {

using namespace WEX::Logging;
using namespace wsl::windows::common;

const TestImage& AlpineTestImage()
{
    static const TestImage image{L"alpine", L"latest", std::filesystem::path{g_testDataPath} / L"alpine-latest.tar"};
    return image;
}

const TestImage& DebianTestImage()
{
    static const TestImage image{L"debian", L"latest", std::filesystem::path{g_testDataPath} / L"debian-latest.tar"};
    return image;
}

const TestImage& PythonTestImage()
{
    static const TestImage image{L"python", L"3.12-alpine", std::filesystem::path{g_testDataPath} / L"python-3_12-alpine.tar"};
    return image;
}

const TestImage& InvalidTestImage()
{
    static const TestImage image{L"mcr.microsoft.com/invalid-image", L"latest", L"INVALID_PATH"};
    return image;
}

void VerifyContainerIsListed(const std::wstring& containerNameOrId, const std::wstring& status)
{
    auto result = RunWslc(L"container list --all");
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message = L"Container '" + containerNameOrId + L"' found in container list output but status '" +
                                         status + L"' was not found in the same line";
            VERIFY_ARE_NOT_EQUAL(std::wstring::npos, line.find(status), message.c_str());
            return;
        }
    }

    const std::wstring message = L"Container '" + containerNameOrId + L"' not found in container list output";
    VERIFY_FAIL(message.c_str());
}

void VerifyImageIsUsed(const TestImage& image)
{
    auto result = RunWslc(L"container list -a");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            return;
        }
    }

    VERIFY_FAIL(std::format(L"Image '{}' not found in container list output", image.NameAndTag()).c_str());
}

void VerifyImageIsNotUsed(const TestImage& image)
{
    auto result = RunWslc(L"container list -a");
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            VERIFY_FAIL(std::format(L"Image '{}' found in container list output", image.NameAndTag()).c_str());
        }
    }
}

std::string GetHashId(const std::string& id, bool fullId)
{
    const int shortIdLength = 12;
    VERIFY_IS_GREATER_THAN_OR_EQUAL(id.length(), shortIdLength);
    if (fullId)
    {
        return id;
    }

    // Remove the "sha256:" prefix if it exists and return the first 12 characters
    const std::string prefix = "sha256:";
    if (id.rfind(prefix, 0) == 0)
    {
        return id.substr(prefix.length(), shortIdLength);
    }

    return id.substr(0, shortIdLength);
}

wsla_schema::InspectContainer InspectContainer(const std::wstring& containerName)
{
    auto result = RunWslc(std::format(L"container inspect {}", containerName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto jsonOutput = result.GetStdoutOneLine();
    auto inspectData = wsl::shared::FromJson<std::vector<wsla_schema::InspectContainer>>(jsonOutput.c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

wsla_schema::InspectImage InspectImage(const std::wstring& imageName)
{
    auto result = RunWslc(std::format(L"image inspect {}", imageName));
    result.Verify({.Stderr = L"", .ExitCode = 0});
    auto jsonOutput = result.GetStdoutOneLine();
    auto inspectData = wsl::shared::FromJson<std::vector<wsla_schema::InspectImage>>(jsonOutput.c_str());
    VERIFY_ARE_EQUAL(1u, inspectData.size());
    return inspectData[0];
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    auto listResult = RunWslc(L"container list --all");
    listResult.Verify({.Stderr = L"", .ExitCode = 0});

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        if (line.find(containerName) != std::wstring::npos)
        {
            if (line.find(L"running") != std::wstring::npos)
            {
                auto result = RunWslc(std::format(L"container kill {}", containerName));
                result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
            }

            auto result = RunWslc(std::format(L"container remove --force {}", containerName));
            result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

void EnsureImageIsDeleted(const TestImage& image)
{
    auto result = RunWslc(L"image list");
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            auto deleteResult = RunWslc(std::format(L"image delete --force {}", image.NameAndTag()));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
            break;
        }
    }
}

void EnsureImageIsLoaded(const TestImage& image)
{
    auto result = RunWslc(L"image list");
    result.Verify({.Stderr = L"", .ExitCode = 0});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(image.NameAndTag()) != std::wstring::npos)
        {
            return;
        }
    }

    // Image not found, load it
    auto loadResult = RunWslc(std::format(L"image load --input {}", image.Path.wstring()));
    loadResult.Verify({.Stderr = L"", .ExitCode = 0});
}
} // namespace WSLCE2ETests