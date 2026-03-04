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

extern std::wstring g_testDataPath;

namespace WSLCE2ETests {

using namespace WEX::Logging;

const TestImage& DebianTestImage()
{
    static const TestImage image{L"debian", L"latest", std::filesystem::path{g_testDataPath} / "debian-latest.tar"};
    return image;
}

const TestImage& InvalidTestImage()
{
    static const TestImage image{L"mcr.microsoft.com/invalid-image", L"latest", "INVALID_PATH"};
    return image;
}

void VerifyContainerIsNotListed(const std::wstring& containerNameOrId)
{
    auto result = RunWslc(L"container list --all");
    result.Verify({.Stderr = L"", .ExitCode = S_OK});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message =
                L"Container '" + containerNameOrId + L"' found in container list output but it should not be listed";
            VERIFY_FAIL(message.c_str());
        }
    }
}

void VerifyContainerIsListed(const std::wstring& containerNameOrId, const std::wstring& status)
{
    auto result = RunWslc(L"container list --all");
    result.Verify({.Stderr = L"", .ExitCode = S_OK});

    auto outputLines = result.GetStdoutLines();
    for (const auto& line : outputLines)
    {
        if (line.find(containerNameOrId) != std::wstring::npos)
        {
            const std::wstring message = L"Container '" + containerNameOrId + L"' found in container list output but status '" +
                                         status + L"' was not found in the same line";
            VERIFY_IS_TRUE(line.find(status) != std::wstring::npos, message.c_str());
            return;
        }
    }

    const std::wstring message = L"Container '" + containerNameOrId + L"' not found in container list output";
    VERIFY_FAIL(message.c_str());
}

void EnsureContainerDoesNotExist(const std::wstring& containerName)
{
    auto listResult = RunWslc(L"container list --all");
    listResult.Verify({.Stderr = L"", .ExitCode = S_OK});

    auto stdoutLines = listResult.GetStdoutLines();
    for (const auto& line : stdoutLines)
    {
        if (line.find(containerName) != std::wstring::npos)
        {
            auto result = RunWslc(L"container delete " + containerName + L" --force");
            result.Verify({.Stderr = L"", .ExitCode = S_OK});
            break;
        }
    }
}

void EnsureImageIsLoaded(const TestImage& image)
{
    auto result = RunWslc(L"image list");
    result.Verify({.Stderr = L"", .ExitCode = S_OK});

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
    loadResult.Verify({.Stderr = L"", .ExitCode = S_OK});
}
} // namespace WSLCE2ETests