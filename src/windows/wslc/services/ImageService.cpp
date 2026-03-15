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
using wsl::windows::common::wsla_schema::InspectImage;

void ImageService::Build(
    wsl::windows::wslc::models::Session& session, const std::wstring& contextPath, const std::wstring& tag, const std::wstring& dockerfilePath, IProgressCallback* callback)
{
    auto absolutePath = std::filesystem::absolute(contextPath);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_DIRECTORY),
        !std::filesystem::is_directory(absolutePath),
        "Path must be a directory: %ls",
        absolutePath.c_str());

    HANDLE dockerfileHandle = nullptr;
    wil::unique_hfile dockerfile;
    if (dockerfilePath == L"-")
    {
        dockerfileHandle = GetStdHandle(STD_INPUT_HANDLE);
    }
    else if (!dockerfilePath.empty())
    {
        dockerfile.reset(CreateFileW(dockerfilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF_MSG(!dockerfile, "Failed to open Dockerfile: %ls", dockerfilePath.c_str());
        dockerfileHandle = dockerfile.get();
    }

    std::string imageTag = tag.empty() ? "" : wsl::windows::common::string::WideToMultiByte(tag);
    THROW_IF_FAILED(session.Get()->BuildImage(
        absolutePath.wstring().c_str(), HandleToULong(dockerfileHandle), imageTag.empty() ? nullptr : imageTag.c_str(), callback));
}

std::vector<ImageInformation> ImageService::List(wsl::windows::wslc::models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLAImageInformation> images;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListImages(nullptr, &images, &count));

    std::vector<ImageInformation> result;
    for (auto ptr = images.get(), end = images.get() + count; ptr != end; ++ptr)
    {
        const WSLAImageInformation& image = *ptr;
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

void ImageService::Delete(wsl::windows::wslc::models::Session& session, const std::string& image, bool force, bool noPrune)
{
    WSLADeleteImageOptions options{};
    options.Image = image.c_str();

    if (force)
    {
        options.Flags |= WSLADeleteImageFlagsForce;
    }

    if (noPrune)
    {
        options.Flags |= WSLADeleteImageFlagsNoPrune;
    }

    wil::unique_cotaskmem_array_ptr<WSLADeletedImageInformation> deletedImages;
    THROW_IF_FAILED(session.Get()->DeleteImage(&options, &deletedImages, deletedImages.size_address<ULONG>()));
}

void ImageService::Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), nullptr, callback));
}

void ImageService::Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage)
{
    auto repoTag = RepoTag::Parse(targetImage);
    WSLATagImageOptions options{};
    options.Image = sourceImage.c_str();
    options.Repo = repoTag.Repo.c_str();
    options.Tag = repoTag.Tag.c_str();
    THROW_IF_FAILED(session.Get()->TagImage(&options));
}

InspectImage ImageService::Inspect(wsl::windows::wslc::models::Session& session, const std::string& image)
{
    wil::unique_cotaskmem_ansistring inspectData;
    THROW_IF_FAILED(session.Get()->InspectImage(image.c_str(), &inspectData));
    return wsl::shared::FromJson<InspectImage>(inspectData.get());
}

void ImageService::Push()
{
}

void ImageService::Save()
{
}

void ImageService::Prune()
{
}
} // namespace wsl::windows::wslc::services
