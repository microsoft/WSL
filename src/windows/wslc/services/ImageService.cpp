/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageService.cpp

Abstract:

    This file contains the ImageService implementation

--*/
#include "ImageService.h"
#include "SessionService.h"
#include <wslutil.h>

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::models;

void ImageService::Build(
    wsl::windows::wslc::models::Session& session, const std::wstring& contextPath, const std::wstring& tag, const std::wstring& dockerfilePath, IProgressCallback* callback)
{
    auto absolutePath = std::filesystem::absolute(contextPath);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_DIRECTORY),
        !std::filesystem::is_directory(absolutePath),
        "Path must be a directory: %ls",
        absolutePath.c_str());

    wil::unique_hfile dockerfileHandle;
    if (dockerfilePath == L"-")
    {
        THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
            GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE), GetCurrentProcess(), &dockerfileHandle, 0, FALSE, DUPLICATE_SAME_ACCESS));
    }
    else if (!dockerfilePath.empty())
    {
        dockerfileHandle.reset(
            CreateFileW(dockerfilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF_MSG(!dockerfileHandle, "Failed to open Dockerfile: %ls", dockerfilePath.c_str());
    }

    std::string imageTag = tag.empty() ? "" : wsl::windows::common::string::WideToMultiByte(tag);
    THROW_IF_FAILED(session.Get()->BuildImage(
        absolutePath.wstring().c_str(), HandleToULong(dockerfileHandle.get()), imageTag.empty() ? nullptr : imageTag.c_str(), callback));
}

std::vector<ImageInformation> ImageService::List(wsl::windows::wslc::models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListImages(nullptr, &images, &count));

    std::vector<ImageInformation> result;
    for (auto ptr = images.get(), end = images.get() + count; ptr != end; ++ptr)
    {
        const WSLA_IMAGE_INFORMATION& image = *ptr;
        ImageInformation info{};
        info.Name = image.Image;
        info.Size = image.Size;
        result.push_back(info);
    }

    return result;
}

void ImageService::Load(wsl::windows::wslc::models::Session& session, const std::wstring& input)
{
    wil::unique_hfile imageFile{CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!imageFile);

    LARGE_INTEGER fileSize{};
    THROW_LAST_ERROR_IF(!GetFileSizeEx(imageFile.get(), &fileSize));
    THROW_IF_FAILED(session.Get()->LoadImage(HandleToULong(imageFile.get()), nullptr, fileSize.QuadPart));
}

void ImageService::Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), nullptr, callback));
}

void ImageService::Push()
{
}

void ImageService::Save()
{
}

void ImageService::Tag()
{
}

void ImageService::Prune()
{
}

void ImageService::Inspect()
{
}

} // namespace wsl::windows::wslc::services
