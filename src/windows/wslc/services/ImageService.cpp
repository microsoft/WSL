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
#include "SpecParsing.h"
#include "SubProcess.h"
#include "WarningCallback.h"
#include <wslutil.h>
#include <HandleConsoleProgressBar.h>
#include <relay.hpp>

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

struct InputSource
{
    InputSource(wsl::windows::common::relay::HandleWrapper&& handle, ULONGLONG contentLength) :
        Handle(std::move(handle)), ContentLength(contentLength)
    {
    }

    wsl::windows::common::relay::HandleWrapper Handle;
    ULONGLONG ContentLength = 0;
};

wsl::windows::common::relay::HandleWrapper OpenInputHandle(const std::wstring& input)
{
    if (input == L"-")
    {
        return wsl::windows::common::relay::HandleWrapper(GetStdHandle(STD_INPUT_HANDLE));
    }

    wil::unique_hfile file(CreateFileW(input.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    THROW_LAST_ERROR_IF(!file);

    return wsl::windows::common::relay::HandleWrapper(std::move(file));
}

InputSource OpenImageInput(const std::wstring& input)
{
    auto handle = OpenInputHandle(input);

    LARGE_INTEGER fileSize{};
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcImportPipeNotSupported(), !GetFileSizeEx(handle.Get(), &fileSize));

    return InputSource{std::move(handle), static_cast<ULONGLONG>(fileSize.QuadPart)};
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
    const std::vector<std::wstring>& labels,
    const std::vector<BuildSecret>& secrets,
    const std::wstring& dockerfilePath,
    const std::wstring& target,
    const std::optional<BuildOutput>& output,
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

    std::vector<std::string> labelStrings;
    std::vector<LPCSTR> labelPointers;
    toMultiByte(labels, labelStrings, labelPointers);

    // Keep narrow-encoded id strings alive for the duration of the COM call. The raw secret bytes are
    // referenced in place from the caller's BuildSecret objects (which outlive this call), so they are
    // never copied or NUL-truncated.
    std::vector<std::string> secretIdStrings;
    std::vector<WSLCBuildSecret> secretEntries;
    secretIdStrings.reserve(secrets.size());
    secretEntries.reserve(secrets.size());
    for (const auto& secret : secrets)
    {
        secretIdStrings.push_back(wsl::windows::common::string::WideToMultiByte(secret.Id));
        secretEntries.push_back(WSLCBuildSecret{
            .Id = secretIdStrings.back().c_str(),
            .Value = secret.Value.empty() ? nullptr : secret.Value.data(),
            .ValueSize = static_cast<ULONG>(secret.Value.size()),
        });
    }

    auto targetStr = wsl::windows::common::string::WideToMultiByte(target);

    // Route the docker-style --output exporter. Exporters that write to a caller-provided destination
    // (tar/oci/docker with dest=, and local) are streamed back out of the VM: the client opens the
    // destination here, forwards the spec with dest= stripped, and the server writes the exporter
    // output to a VM temp path then streams it to OutputHandle. Exporters with no client destination
    // (docker load, image, registry, cacheonly) run entirely in the VM and the spec is forwarded as-is.
    std::string outputStr;
    HANDLE outputHandle = nullptr;
    wil::unique_hfile outputFile;

    // For type=local the server streams a tarball of the export directory; the client extracts it into
    // the destination directory by feeding the stream to `tar.exe -xf -`. These stay alive until after
    // the (synchronous) BuildImage call so the pipe keeps draining while the server writes.
    wil::unique_hfile extractPipeWrite;
    wil::unique_handle extractProcess;

    if (output.has_value())
    {
        const auto& spec = output.value();
        const bool isFileExporter = spec.Type == L"tar" || spec.Type == L"oci" || (spec.Type == L"docker" && !spec.Dest.empty());
        const bool isDirExporter = spec.Type == L"local";

        if (isFileExporter)
        {
            if (spec.Dest == L"-")
            {
                // dest=- streams the exporter tarball to the client's stdout, matching docker.
                outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);

                // Refuse to dump the binary exporter stream onto an interactive console (matching docker);
                // stdout must be redirected to a file or pipe. Otherwise ToCOMInputHandle would fail the
                // marshal with a cryptic ERROR_NOT_SUPPORTED for the FILE_TYPE_CHAR console handle.
                THROW_HR_WITH_USER_ERROR_IF(
                    E_INVALIDARG,
                    Localization::MessageWslcOutputInvalidSpec(
                        validation::FormatOutputSpec(spec),
                        L"refusing to write build output to the console; redirect stdout to a file or pipe (for example "
                        L"'> out.tar') or pass 'dest=' with a file path"),
                    GetFileType(outputHandle) == FILE_TYPE_CHAR);
            }
            else
            {
                outputFile.reset(CreateFileW(
                    spec.Dest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
                THROW_LAST_ERROR_IF_MSG(!outputFile, "Failed to create output file: %ls", spec.Dest.c_str());
                outputHandle = outputFile.get();
            }

            // The server picks the VM-side dest, so forward the spec without the client's dest.
            BuildOutput vmSpec = spec;
            vmSpec.Dest.clear();
            outputStr = wsl::windows::common::string::WideToMultiByte(validation::FormatOutputSpec(vmSpec));
        }
        else if (isDirExporter)
        {
            std::error_code dirError;
            std::filesystem::create_directories(std::filesystem::absolute(spec.Dest), dirError);
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(dirError.value()), !!dirError, "Failed to create directory: %ls", spec.Dest.c_str());

            // Strip any trailing separator so the CRT does not parse '\"' as an escaped quote.
            auto destDir = std::filesystem::absolute(spec.Dest).wstring();
            while (destDir.size() > 1 && (destDir.back() == L'\\' || destDir.back() == L'/'))
            {
                destDir.pop_back();
            }

            auto [pipeRead, pipeWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, false, false);
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(pipeRead.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            auto tarCmd = std::format(L"tar.exe -xf - -C \"{}\"", destDir);
            wsl::windows::common::SubProcess tarProcess(nullptr, tarCmd.c_str());
            tarProcess.SetStdHandles(pipeRead.get(), nullptr, nullptr);
            extractProcess = tarProcess.Start();
            pipeRead.reset();

            extractPipeWrite = std::move(pipeWrite);
            outputHandle = extractPipeWrite.get();
            WI_SetFlag(flags, WSLCBuildImageFlagsOutputIsDirectory);

            // The server picks the VM-side export directory, so forward the spec without the client's dest.
            BuildOutput vmSpec = spec;
            vmSpec.Dest.clear();
            outputStr = wsl::windows::common::string::WideToMultiByte(validation::FormatOutputSpec(vmSpec));
        }
        else
        {
            outputStr = wsl::windows::common::string::WideToMultiByte(validation::FormatOutputSpec(spec));
        }
    }

    auto contextPathStr = absolutePath.wstring();
    WSLCBuildImageOptions options{
        .ContextPath = contextPathStr.c_str(),
        .DockerfileHandle = ToCOMInputHandle(dockerfileHandle),
        .Tags = {tagPointers.data(), static_cast<ULONG>(tagPointers.size())},
        .BuildArgs = {buildArgPointers.data(), static_cast<ULONG>(buildArgPointers.size())},
        .Target = targetStr.empty() ? nullptr : targetStr.c_str(),
        .Flags = flags,
        .Labels = {labelPointers.data(), static_cast<ULONG>(labelPointers.size())},
        .Secrets = {secretEntries.data(), static_cast<ULONG>(secretEntries.size())},
        .Output = outputStr.empty() ? nullptr : outputStr.c_str(),
        .OutputHandle = outputHandle != nullptr ? ToCOMInputHandle(outputHandle) : WSLCHandle{.Type = WSLCHandleTypeUnknown},
    };

    THROW_IF_FAILED(session.Get()->BuildImage(&options, callback, cancelEvent));

    // For type=local, signal EOF to tar.exe (the server has finished writing) and confirm it extracted
    // the stream cleanly. The write end must be reset before waiting so tar sees end-of-input.
    if (extractProcess)
    {
        extractPipeWrite.reset();
        auto exitCode = wsl::windows::common::SubProcess::GetExitCode(extractProcess.get());
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0, "tar.exe exited with code %u", exitCode);
    }
}

std::vector<ImageInformation> ImageService::List(
    wsl::windows::wslc::models::Session& session, const std::vector<std::pair<std::string, std::string>>& filters)
{
    std::vector<WSLCFilter> filterEntries;
    filterEntries.reserve(filters.size());
    for (const auto& [key, value] : filters)
    {
        filterEntries.push_back({.Key = key.c_str(), .Value = value.c_str()});
    }

    WSLCListImagesOptions options{};
    options.Flags = WSLCListImagesFlagsNone;
    options.Filters = filterEntries.empty() ? nullptr : filterEntries.data();
    options.FiltersCount = static_cast<ULONG>(filterEntries.size());

    wil::unique_cotaskmem_array_ptr<WSLCImageInformation> images;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListImages(&options, &images, &count));

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

void ImageService::Load(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::wstring& input, IImageLoadCallback* callback)
{
    WarningCallback warningCallback(reporter);
    auto source = OpenImageInput(input);
    THROW_IF_FAILED(session.Get()->LoadImage(ToCOMInputHandle(source.Handle.Get()), source.ContentLength, &warningCallback, callback));
}

std::string ImageService::Import(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::wstring& input, const std::string& imageName)
{
    WarningCallback warningCallback(reporter);
    auto source = OpenImageInput(input);
    wil::unique_cotaskmem_ansistring imageId;
    THROW_IF_FAILED(session.Get()->ImportImage(
        ToCOMInputHandle(source.Handle.Get()), imageName.empty() ? nullptr : imageName.c_str(), source.ContentLength, &warningCallback, &imageId));
    return imageId.get() ? std::string(imageId.get()) : std::string();
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

void ImageService::Pull(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    WarningCallback warningCallback(reporter);
    auto server = GetServerFromImage(image);
    auto auth = RegistryService::Get(server);
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), auth.c_str(), callback, &warningCallback));
}

void ImageService::Tag(wsl::windows::wslc::models::Session& session, const std::string& sourceImage, const std::string& targetImage)
{
    EnumReferenceFormat format;
    auto [repo, tag] = ParseImage(targetImage, &format);
    if (format == EnumReferenceFormatDigest)
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

void ImageService::Push(Reporter& reporter, wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    WarningCallback warningCallback(reporter);
    auto server = GetServerFromImage(image);
    auto auth = RegistryService::Get(server);
    THROW_IF_FAILED(session.Get()->PushImage(image.c_str(), auth.c_str(), callback, &warningCallback));
}

void ImageService::Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, const std::wstring& output, HANDLE cancelEvent)
{
    wil::unique_hfile outputFile{
        CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    THROW_LAST_ERROR_IF(!outputFile);

    Save(session, images, outputFile.get(), cancelEvent);
}

void ImageService::Save(wsl::windows::wslc::models::Session& session, const std::vector<std::string>& images, HANDLE outputHandle, HANDLE cancelEvent)
{
    WI_ASSERT(!images.empty());

    wsl::windows::common::HandleConsoleProgressBar progressBar(
        outputHandle, Localization::MessageWslcSaveInProgress(), wsl::windows::common::HandleConsoleProgressBar::Format::FileSize);

    if (images.size() == 1)
    {
        THROW_IF_FAILED(session.Get()->SaveImage(ToCOMInputHandle(outputHandle), images[0].c_str(), nullptr, cancelEvent));
    }
    else
    {
        std::vector<LPCSTR> imagePointers;
        imagePointers.reserve(images.size());
        for (const auto& image : images)
        {
            imagePointers.push_back(image.c_str());
        }

        WSLCStringArray imageArray{
            .Values = imagePointers.data(),
            .Count = static_cast<ULONG>(imagePointers.size()),
        };

        THROW_IF_FAILED(session.Get()->SaveImages(ToCOMInputHandle(outputHandle), &imageArray, nullptr, cancelEvent));
    }
}

wsl::windows::wslc::models::PruneImagesResult ImageService::Prune(
    wsl::windows::wslc::models::Session& session, bool all, const std::vector<std::pair<std::string, std::string>>& filters)
{
    // The --all flag is translated into a `dangling` filter. Skip the implicit
    // filter if the caller already supplied an explicit `dangling` filter so the
    // user's value wins (matching docker's behavior).
    const bool hasExplicitDangling =
        std::any_of(filters.begin(), filters.end(), [](const auto& f) { return f.first == "dangling"; });

    std::vector<WSLCFilter> filterEntries;
    filterEntries.reserve(filters.size() + (hasExplicitDangling ? 0 : 1));
    if (!hasExplicitDangling)
    {
        filterEntries.push_back({.Key = "dangling", .Value = all ? "false" : "true"});
    }

    for (const auto& [key, value] : filters)
    {
        filterEntries.push_back({.Key = key.c_str(), .Value = value.c_str()});
    }

    wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
    ULONGLONG spaceReclaimed = 0;
    THROW_IF_FAILED(session.Get()->PruneImages(
        filterEntries.data(), static_cast<ULONG>(filterEntries.size()), &deletedImages, deletedImages.size_address<ULONG>(), &spaceReclaimed));

    wsl::windows::wslc::models::PruneImagesResult result;
    result.SpaceReclaimed = spaceReclaimed;
    for (auto ptr = deletedImages.get(), end = deletedImages.get() + deletedImages.size(); ptr != end; ++ptr)
    {
        if (ptr->Type == WSLCDeletedImageTypeDeleted)
        {
            result.DeletedImages.push_back(ptr->Image);
        }
        else
        {
            result.UntaggedImages.push_back(ptr->Image);
        }
    }

    return result;
}
} // namespace wsl::windows::wslc::services
