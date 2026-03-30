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
using wsl::windows::common::wslc_schema::InspectImage;

void ImageService::Build(
    wsl::windows::wslc::models::Session& session,
    const std::wstring& contextPath,
    const std::vector<std::wstring>& tags,
    const std::vector<std::wstring>& buildArgs,
    const std::wstring& dockerfilePath,
    bool verbose,
    IProgressCallback* callback)
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

    auto toMultiByte = [](const std::vector<std::wstring>& input, std::vector<std::string>& strings, std::vector<LPCSTR>& pointers) {
        strings.reserve(input.size());
        for (const auto& s : input)
        {
            strings.push_back(wsl::windows::common::string::WideToMultiByte(s));
            pointers.push_back(strings.back().c_str());
        }
    };

    std::vector<std::string> tagStrings;
    std::vector<LPCSTR> tagPointers;
    toMultiByte(tags, tagStrings, tagPointers);

    std::vector<std::string> buildArgStrings;
    std::vector<LPCSTR> buildArgPointers;
    toMultiByte(buildArgs, buildArgStrings, buildArgPointers);

    auto contextPathStr = absolutePath.wstring();
    WSLCBuildImageOptions options{
        .ContextPath = contextPathStr.c_str(),
        .DockerfileHandle = HandleToULong(dockerfileHandle),
        .Tags = {tagPointers.data(), static_cast<ULONG>(tagPointers.size())},
        .BuildArgs = {buildArgPointers.data(), static_cast<ULONG>(buildArgPointers.size())},
        .Verbose = verbose,
    };

    THROW_IF_FAILED(session.Get()->BuildImage(&options, callback));
}

std::vector<ImageInformation> ImageService::List(wsl::windows::wslc::models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListImages(nullptr, &images, &count));

    std::vector<ImageInformation> result;
    for (auto ptr = images.get(), end = images.get() + count; ptr != end; ++ptr)
    {
        const WSLCImageInformation& image = *ptr;
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
    WSLCDeleteImageOptions options{};
    options.Image = image.c_str();

    if (force)
    {
        options.Flags |= WSLCDeleteImageFlagsForce;
    }

    if (noPrune)
    {
        options.Flags |= WSLCDeleteImageFlagsNoPrune;
    }

    wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
    THROW_IF_FAILED(session.Get()->DeleteImage(&options, &deletedImages, deletedImages.size_address<ULONG>()));
}

void ImageService::Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), nullptr, callback));
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

void ImageService::Save(wsl::windows::wslc::models::Session& session, const std::string& image, const std::wstring& output)
{
    wil::unique_hfile outputFile{CreateFileW(
        output.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!outputFile);

    // TODO Handle Ctrl-C and progress callback
    THROW_IF_FAILED(session.Get()->SaveImage(HandleToULong(outputFile.get()), image.c_str(), nullptr, nullptr));
}

void ImageService::Tag()
{
}

void ImageService::Prune()
{
}
} // namespace wsl::windows::wslc::services
