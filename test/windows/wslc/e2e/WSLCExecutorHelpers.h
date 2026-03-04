/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCExecutorHelpers.h

Abstract:

    This file contains helper functions for WSLCExecutor tests.
--*/

#pragma once

#include "WSLCExecutor.h"

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

TestImage GetDebianTestImageInfo();
TestImage GetInvalidTestImageInfo();
void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void VerifyContainerIsNotListed(const std::wstring& containerNameOrId);
void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image);

} // namespace WSLCE2ETests