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

void ImageService::Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), nullptr, callback));
}

void ImageService::Load(wsl::windows::wslc::models::Session& session, const wil::unique_hfile& imageFile)
{
    LARGE_INTEGER fileSize{};
    THROW_LAST_ERROR_IF(!GetFileSizeEx(imageFile.get(), &fileSize));

    // TODO add callback support for load operation
    THROW_IF_FAILED(session.Get()->LoadImage(HandleToULong(imageFile.get()), nullptr, fileSize.QuadPart));
}

void ImageService::Load(wsl::windows::wslc::models::Session& session, const std::string& input)
{
    // Check if the input is a valid file path.
    if (!std::filesystem::exists(input))
    {
        THROW_HR(E_INVALIDARG);
    }

    std::filesystem::path inputPath(input);
    wil::unique_hfile imageFile{
        CreateFileW(inputPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!imageFile);
    Load(session, imageFile);
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
