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

template <typename TInterval = std::chrono::milliseconds, typename TTimeout = std::chrono::milliseconds>
void VerifyContainerIsNotListed(const std::wstring& containerNameOrId, TInterval retryInterval = TInterval(0), TTimeout timeout = TTimeout(0));

void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::docker_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::docker_schema::InspectImage InspectImage(const std::wstring& imageName);

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image);
void EnsureImageIsDeleted(const TestImage& image);
} // namespace WSLCE2ETests