/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageService.cpp

Abstract:

    This file contains the ImageService implementation

--*/
#include "ImageService.h"
#include "RegistryService.h"
#include "SessionService.h"
#include <wslutil.h>
#include <HandleConsoleProgressBar.h>

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;

namespace {

wil::unique_hfile ResolveBuildFile(const std::filesystem::path& contextPath)
{
    auto containerfilePath = contextPath / L"Containerfile";
    auto containerfileStatus = wil::try_open_file(containerfilePath.c_str());

    auto dockerfilePath = contextPath / L"Dockerfile";
    auto dockerfileStatus = wil::try_open_file(dockerfilePath.c_str());

    // Fail if both Containerfile and Dockerfile exist.
    // Assume that both exist if one opens successfully and the other returns anything other than ERROR_FILE_NOT_FOUND to cover the case where one of them exists, but fails to open.
    // If both exist but fail to open, the logic after this block will report the appropriate error.
    if ((containerfileStatus.last_error != ERROR_FILE_NOT_FOUND && dockerfileStatus.file) ||
        (dockerfileStatus.last_error != ERROR_FILE_NOT_FOUND && containerfileStatus.file))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcBothDockerAndContainerFileFound());
    }

    if (containerfileStatus.last_error != ERROR_FILE_NOT_FOUND)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(containerfileStatus.last_error),
            Localization::MessageWslcFailedToOpenFile(
                containerfilePath, wsl::windows::common::wslutil::GetSystemErrorString(HRESULT_FROM_WIN32(containerfileStatus.last_error))),
            !containerfileStatus.file.is_valid());

        return std::move(containerfileStatus.file);
    }

    if (dockerfileStatus.last_error != ERROR_FILE_NOT_FOUND)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(dockerfileStatus.last_error),
            Localization::MessageWslcFailedToOpenFile(
                dockerfilePath, wsl::windows::common::wslutil::GetSystemErrorString(HRESULT_FROM_WIN32(dockerfileStatus.last_error))),
            !dockerfileStatus.file.is_valid());

        return std::move(dockerfileStatus.file);
    }

    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcBuildFileNotFound(contextPath));
}

std::string GetServerFromImage(const std::string& image)
{
    auto [repo, tag] = wsl::windows::common::wslutil::ParseImage(image);
    auto [server, path] = wsl::windows::common::wslutil::NormalizeRepo(repo);
    return server;
}

} // namespace

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::models;
using wsl::windows::common::wslc_schema::InspectImage;

void ImageService::Build(
    wsl::windows::wslc::models::Session& session,
    const std::wstring& contextPath,
    const std::vector<std::wstring>& tags,
    const std::vector<std::wstring>& buildArgs,
    const std::wstring& dockerfilePath,
    const std::wstring& target,
    WSLCBuildImageFlags flags,
    IProgressCallback* callback,
    HANDLE cancelEvent)
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
    else
    {
        dockerfile = ResolveBuildFile(absolutePath);
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

    auto targetStr = wsl::windows::common::string::WideToMultiByte(target);

    auto contextPathStr = absolutePath.wstring();
    WSLCBuildImageOptions options{
        .ContextPath = contextPathStr.c_str(),
        .DockerfileHandle = ToCOMInputHandle(dockerfileHandle),
        .Tags = {tagPointers.data(), static_cast<ULONG>(tagPointers.size())},
        .BuildArgs = {buildArgPointers.data(), static_cast<ULONG>(buildArgPointers.size())},
        .Target = targetStr.empty() ? nullptr : targetStr.c_str(),
        .Flags = flags,
    };

    THROW_IF_FAILED(session.Get()->BuildImage(&options, callback, cancelEvent));
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

        // Parse the image reference — dangling images have no repo/tag
        std::string imageRef = image.Image;
        if (imageRef != "<none>:<none>")
        {
            auto parsed = wsl::windows::common::wslutil::ParseImage(imageRef);
            info.Repository = parsed.first;
            info.Tag = parsed.second;
        }

        info.Id = image.Hash;
        info.Created = image.Created;
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

    THROW_IF_FAILED(session.Get()->LoadImage(ToCOMInputHandle(imageFile.get()), nullptr, fileSize.QuadPart));
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
    auto server = GetServerFromImage(image);
    auto auth = RegistryService::Get(server);
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), auth.c_str(), callback));
}

void ImageService::Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage)
{
    EnumReferenceFormat format;
    auto [repo, tag] = ParseImage(targetImage, &format);
    if (format == EnumReferenceFormat::Digest)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcTagImageInvalidFormat(targetImage.c_str()));
    }

    WSLCTagImageOptions options{};
    options.Image = sourceImage.c_str();
    options.Repo = repo.c_str();
    options.Tag = tag ? tag->c_str() : "";

    THROW_IF_FAILED(session.Get()->TagImage(&options));
}

InspectImage ImageService::Inspect(wsl::windows::wslc::models::Session& session, const std::string& image)
{
    wil::unique_cotaskmem_ansistring inspectData;
    THROW_IF_FAILED(session.Get()->InspectImage(image.c_str(), &inspectData));
    return wsl::shared::FromJson<InspectImage>(inspectData.get());
}

void ImageService::Push(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    auto server = GetServerFromImage(image);
    auto auth = RegistryService::Get(server);
    THROW_IF_FAILED(session.Get()->PushImage(image.c_str(), auth.c_str(), callback));
}

void ImageService::Save(wsl::windows::wslc::models::Session& session, const std::string& image, const std::wstring& output, HANDLE cancelEvent)
{
    wil::unique_hfile outputFile{
        CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!outputFile);

    Save(session, image, outputFile.get(), cancelEvent);
}

void ImageService::Save(wsl::windows::wslc::models::Session& session, const std::string& image, HANDLE outputHandle, HANDLE cancelEvent)
{
    wsl::windows::common::HandleConsoleProgressBar progressBar(
        outputHandle, L"Save in progress.", wsl::windows::common::HandleConsoleProgressBar::Format::FileSize);
    THROW_IF_FAILED(session.Get()->SaveImage(ToCOMInputHandle(outputHandle), image.c_str(), nullptr, cancelEvent));
}

void ImageService::Prune()
{
}
} // namespace wsl::windows::wslc::services
