/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EHelpers.h

Abstract:

    This file contains helper functions for WSLCE2E tests.
--*/

#pragma once

#include "WSLCExecutor.h"
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

const TestImage& AlpineTestImage();
const TestImage& DebianTestImage();
const TestImage& InvalidTestImage();

void VerifyContainerIsListed(const std::wstring& containerName, const std::wstring& status);
void VerifyContainerIsNotListed(const std::wstring& containerNameOrId);
void VerifyImageIsUsed(const TestImage& image);
void VerifyImageIsNotUsed(const TestImage& image);

std::string GetHashId(const std::string& id, bool fullId = false);
wsl::windows::common::wsla_schema::InspectContainer InspectContainer(const std::wstring& containerName);
wsl::windows::common::wsla_schema::InspectImage InspectImage(const std::wstring& imageName);

void EnsureContainerDoesNotExist(const std::wstring& containerName);
void EnsureImageIsLoaded(const TestImage& image);
void EnsureImageIsDeleted(const TestImage& image);
} // namespace WSLCE2ETests