/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.h

Abstract:

    This file contains helper functions for WSLCE2E tests.
--*/

#pragma once

#include "WSLCExecutor.h"
#include <docker_schema.h>
#include <chrono>
#include <wsla_schema.h>

namespace WSLCE2ETests {

struct TestImage
{
    std::wstring Name;
    std::wstring Tag;
    std::filesystem::path Path;
    std::wstring NameAndTag() const
    {
        return std::format(L"{}:{}", Name, Tag);
    }
};

const TestImage& DebianTestImage();
const TestImage& InvalidTestImage();

void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::wsla_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::wsla_schema::InspectImage InspectImage(const std::wstring& imageName);

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image);
void EnsureImageIsDeleted(const TestImage& image);

// Default timeout of 0 will execute once.
template <typename TInterval, typename TTimeout>
void VerifyContainerIsNotListed(const std::wstring& containerNameOrId, TInterval retryInterval, TTimeout timeout)
{
    try
    {
        wsl::shared::retry::RetryWithTimeout<void>(
            [&containerNameOrId]() {
                auto result = RunWslc(L"container list --all");
                result.Verify({.Stderr = L"", .ExitCode = 0});

                auto outputLines = result.GetStdoutLines();
                for (const auto& line : outputLines)
                {
                    if (line.find(containerNameOrId) != std::wstring::npos)
                    {
                        THROW_HR(E_FAIL);
                    }
                }
            },
            retryInterval,
            timeout);
    }
    catch (...)
    {
        HRESULT hr = wil::ResultFromCaughtException();
        const bool hasTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count() > 0;
        const std::wstring message =
            hr == E_FAIL
                ? std::format(L"Container '{}' found in container list output{}", containerNameOrId, hasTimeout ? L" after timeout" : L" but it should not be listed")
                : std::format(
                      L"Unexpected error while verifying container '{}' is not listed: 0x{:08X}",
                      containerNameOrId,
                      static_cast<unsigned int>(hr));
        VERIFY_FAIL(message.c_str());
    }
}

inline void VerifyContainerIsNotListed(const std::wstring& containerNameOrId)
{
    VerifyContainerIsNotListed(containerNameOrId, std::chrono::milliseconds(0), std::chrono::milliseconds(0));
}
} // namespace WSLCE2ETests