/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTasks.cpp

Abstract:

    Implementation of image command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "BuildImageCallback.h"
#include "CLIExecutionContext.h"
#include "ContainerService.h"
#include "ImageModel.h"
#include "ImageService.h"
#include "ImageTasks.h"
#include "ImageProgressCallback.h"
#include "TableOutput.h"
#include "Task.h"
#include <format>
#include <fstream>
#include <wslutil.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {

namespace {

    class DECLSPEC_UUID("91EF98A7-99A8-41C2-893C-43CDFB7DB69F") WSLCImageLoadCallback
        : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IImageLoadCallback, IFastRundown>
    {
    public:
        explicit WSLCImageLoadCallback(Reporter& reporter) : m_reporter(reporter)
        {
        }

        HRESULT OnImageLoaded(LPCSTR Reference, EnumReferenceFormat Format) override
        try
        {
            if (Format == EnumReferenceFormatDigest)
            {
                m_reporter.Output(L"{}\n", Localization::WSLCCLI_ImageLoadedId(Reference));
            }
            else if (Format == EnumReferenceFormatTag)
            {
                m_reporter.Output(L"{}\n", Localization::WSLCCLI_ImageLoaded(Reference));
            }
            else
            {
                THROW_HR_MSG(E_UNEXPECTED, "Unexpected reference type: %d, '%hs'", Format, Reference);
            }

            return S_OK;
        }
        CATCH_RETURN();

    private:
        Reporter& m_reporter;
    };

} // namespace

static services::BuildSecret ParseSecretSpec(const std::wstring& spec)
{
    // The spec was already validated by validation::ValidateSecretSpec during argument processing, so
    // this only parses the (known-valid) spec and resolves the secret's bytes.
    std::wstring id;
    std::wstring type;
    std::wstring envName;
    std::wstring srcPath;

    for (const auto& part : wsl::shared::string::Split(spec, L','))
    {
        auto eq = part.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
        {
            continue;
        }
        auto key = part.substr(0, eq);
        auto value = part.substr(eq + 1);

        if (key == L"id")
        {
            id = value;
        }
        else if (key == L"type")
        {
            type = value;
        }
        else if (key == L"env")
        {
            envName = value;
        }
        else if (key == L"src" || key == L"source")
        {
            srcPath = value;
        }
    }

    // Docker parity: with 'type=env', a bare 'src=' names the environment variable to read (rather
    // than a file path), unless an explicit 'env=' was also given.
    if (type == L"env" && envName.empty() && !srcPath.empty())
    {
        envName = std::move(srcPath);
        srcPath.clear();
    }

    if (!envName.empty() && !srcPath.empty())
    {
        // Docker parity: 'env=' and 'src=' are not mutually exclusive; when both are given the
        // environment variable wins and the file path is ignored.
        srcPath.clear();
    }
    if (envName.empty() && srcPath.empty())
    {
        // Docker parity: with neither 'env=' nor 'src=', the secret value is read from the host
        // environment variable whose name matches the id.
        envName = id;
    }

    if (!srcPath.empty())
    {
        std::error_code ec;
        // Resolve symlinks (and normalize '..') so we read the file that actually holds the secret's
        // bytes rather than the link node itself.
        auto absPath = std::filesystem::weakly_canonical(std::filesystem::absolute(srcPath), ec);

        // Read the file's raw bytes and forward them verbatim. The server materializes them into a
        // root-only tmpfs file inside the VM, so file secrets are byte-exact (binary, embedded NULs,
        // and arbitrary size all round-trip) - matching Docker's type=file semantics - without ever
        // mounting a host directory into the VM.
        std::ifstream file(absPath, std::ios::binary | std::ios::ate);
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unable to open source file: {}", absPath.wstring())),
            !file);

        // Read a known number of bytes rather than draining via istreambuf_iterator: that iterator
        // cannot distinguish EOF from a mid-stream read error, so a transient I/O failure would
        // silently truncate the secret. Size the buffer from the stream, read exactly that many
        // bytes, then verify the full contents were delivered so a short read is surfaced as an
        // error instead of forwarding a partial secret to the build.
        const std::streamoff size = file.tellg();
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unable to determine size of source file: {}", absPath.wstring())),
            size < 0);

        std::vector<BYTE> value(static_cast<size_t>(size));
        if (size > 0)
        {
            file.seekg(0);
            file.read(reinterpret_cast<char*>(value.data()), size);
            THROW_HR_WITH_USER_ERROR_IF(
                E_UNEXPECTED,
                Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"failed to read source file: {}", absPath.wstring())),
                file.bad() || file.gcount() != size);
        }

        return services::BuildSecret{
            .Id = std::move(id),
            .Value = std::move(value),
        };
    }

    // Docker parity: a referenced environment variable that is unset (or set but empty) yields an
    // empty secret value rather than an error. GetEnvironmentVariableW returns 0 for an undefined
    // variable; for a defined one it returns the buffer size needed including the null terminator.
    std::wstring value;
    DWORD size = GetEnvironmentVariableW(envName.c_str(), nullptr, 0);
    if (size > 0)
    {
        value.resize(size);
        DWORD written = GetEnvironmentVariableW(envName.c_str(), value.data(), size);
        // If the variable grew between the size query above and this read, GetEnvironmentVariableW
        // returns the newly-required size (>= our buffer) without filling it; treat that as an error
        // rather than forwarding a truncated/garbage secret value to the build.
        THROW_HR_IF(E_UNEXPECTED, written >= size);
        value.resize(written);
    }

    // The env value is delivered as UTF-8 bytes, matching how the guest exposes it at /run/secrets/<id>.
    auto valueBytes = wsl::windows::common::string::WideToMultiByte(value);
    return services::BuildSecret{
        .Id = std::move(id),
        .Value = std::vector<BYTE>(valueBytes.begin(), valueBytes.end()),
    };
}

static bool TryInspectImage(Reporter& reporter, Session& session, const std::string& imageId, std::optional<wslc_schema::InspectImage>& inspectData)
{
    try
    {
        inspectData = ImageService::Inspect(session, imageId);
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_IMAGE_NOT_FOUND)
        {
            reporter.Error(L"{}\n", Localization::MessageWslcImageNotFound(imageId.c_str()));
            return false;
        }

        throw;
    }
}

void BuildImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::Path));
    auto& session = context.Data.Get<Data::Session>();
    auto& contextPath = context.Args.Get<ArgType::Path>();

    auto tags = context.Args.GetAll<ArgType::Tag>();
    auto buildArgs = context.Args.GetAll<ArgType::BuildArg>();
    auto labels = context.Args.GetAll<ArgType::Label>();
    for (const auto& label : labels)
    {
        validation::ParseLabel(label);
    }

    std::vector<services::BuildSecret> secrets;
    if (context.Args.Contains(ArgType::Secret))
    {
        for (const auto& spec : context.Args.GetAll<ArgType::Secret>())
        {
            secrets.push_back(ParseSecretSpec(spec));
        }
    }

    std::wstring dockerfilePath;
    if (context.Args.Contains(ArgType::File))
    {
        dockerfilePath = context.Args.Get<ArgType::File>();
    }

    std::wstring target;
    if (context.Args.Contains(ArgType::BuildTarget))
    {
        target = context.Args.Get<ArgType::BuildTarget>();
    }

    WSLCBuildImageFlags flags = WSLCBuildImageFlagsNone;
    WI_SetFlagIf(flags, WSLCBuildImageFlagsVerbose, context.Args.Contains(ArgType::Verbose));
    WI_SetFlagIf(flags, WSLCBuildImageFlagsNoCache, context.Args.Contains(ArgType::NoCache));
    WI_SetFlagIf(flags, WSLCBuildImageFlagsPull, context.Args.Contains(ArgType::BuildPull));

    auto cancelEvent = context.CreateCancelEvent();
    BuildImageCallback callback(context.Reporter, cancelEvent, context.Args.Contains(ArgType::Verbose));
    services::ImageService::Build(session, contextPath, tags, buildArgs, labels, secrets, dockerfilePath, target, flags, &callback, cancelEvent);
}

void GetImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    // Filter syntax (`key=value`) is enforced upstream; here we just split on the first '='.
    std::vector<std::pair<std::string, std::string>> filters;
    if (context.Args.Contains(ArgType::Filter))
    {
        for (const auto& wideValue : context.Args.GetAll<ArgType::Filter>())
        {
            std::string raw = WideToMultiByte(wideValue);
            const auto eq = raw.find('=');
            WI_ASSERT(eq != std::string::npos);

            filters.emplace_back(raw.substr(0, eq), raw.substr(eq + 1));
        }
    }

    auto images = ImageService::List(session, filters);
    context.Data.Add<Data::Images>(std::move(images));
}

void ListImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Images));
    auto& images = context.Data.Get<Data::Images>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);
        for (const auto& image : images)
        {
            context.Reporter.Output(L"{}\n", trunc ? TruncateId(image.Id, true) : image.Id);
        }

        return;
    }

    FormatType format = FormatType::Table; // Default is table
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(images, c_jsonPrettyPrintIndent);
        context.Reporter.Output(L"{}\n", MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);
        using enum ColumnOverflow;

        // Create table — only IMAGE ID uses fixed width; other columns shrink to fit the console.
        // When --no-trunc is passed, IMAGE ID also shows full length via TruncateId().
        auto table =
            trunc
                ? wsl::windows::wslc::TableOutput<5>(
                      context.Reporter,
                      {{{L"REPOSITORY", {.Overflow = Shrink}},
                        {L"TAG", {.Overflow = Shrink}},
                        {L"IMAGE ID", {.MinWidth = 12, .MaxWidth = 12, .Overflow = Shrink}},
                        {L"CREATED", {.Overflow = Shrink}},
                        {L"SIZE", {.Overflow = Shrink}}}},
                      images.size())
                : wsl::windows::wslc::TableOutput<5>(context.Reporter, {L"REPOSITORY", L"TAG", L"IMAGE ID", L"CREATED", L"SIZE"});

        for (const auto& image : images)
        {
            table.WriteRow({
                MultiByteToWide(image.Repository.value_or("<untagged>")),
                MultiByteToWide(image.Tag.value_or("<untagged>")),
                MultiByteToWide(TruncateId(image.Id, trunc)),
                ContainerService::FormatRelativeTime(image.Created > 0 ? static_cast<ULONGLONG>(image.Created) : 0),
                std::format(L"{:.2f} MB", static_cast<double>(image.Size) / WSLC_IMAGE_1MB),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void PullImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.Get<ArgType::ImageId>();

    ImageProgressCallback callback(context.Reporter, Reporter::Level::Output);
    services::ImageService::Pull(context.Reporter, session, WideToMultiByte(imageId), &callback);
}

void PushImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.Get<ArgType::ImageId>();

    ImageProgressCallback callback(context.Reporter, Reporter::Level::Output);
    services::ImageService::Push(context.Reporter, session, WideToMultiByte(imageId), &callback);
}

void DeleteImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    const auto& imageIds = context.Args.GetAll<ArgType::ImageId>();
    bool force = context.Args.Contains(ArgType::ImageForce);
    bool noPrune = context.Args.Contains(ArgType::NoPrune);
    for (const auto& id : imageIds)
    {
        services::ImageService::Delete(session, WideToMultiByte(id), force, noPrune);
    }
}

void LoadImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    if (context.Args.Contains(ArgType::Input))
    {
        auto& input = context.Args.Get<ArgType::Input>();
        auto callback = wil::MakeOrThrow<WSLCImageLoadCallback>(context.Reporter);
        services::ImageService::Load(context.Reporter, session, input, callback.Get());
        return;
    }

    // TODO Read from stdin if no input argument is provided.
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_ImageLoadNoInputError());
}

void ImportImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImportFile));
    auto& session = context.Data.Get<Data::Session>();

    std::string imageName;
    if (context.Args.Contains(ArgType::ImageId))
    {
        imageName = WideToMultiByte(context.Args.Get<ArgType::ImageId>());
    }

    auto& input = context.Args.Get<ArgType::ImportFile>();
    auto imageId = services::ImageService::Import(context.Reporter, session, input, imageName);
    if (!imageId.empty())
    {
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);
        context.Reporter.Output(L"{}\n", MultiByteToWide(TruncateId(imageId, trunc)));
    }
}

void InspectImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto imageIds = context.Args.GetAll<ArgType::ImageId>();

    std::vector<wsl::windows::common::wslc_schema::InspectImage> result;
    for (const auto& id : imageIds)
    {
        std::optional<wslc_schema::InspectImage> inspectData;
        if (TryInspectImage(context.Reporter, session, WideToMultiByte(id), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    context.Reporter.Output(L"{}\n", MultiByteToWide(json));
}

void SaveImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto imageIds = context.Args.GetAll<ArgType::ImageId>();

    std::vector<std::string> images;
    images.reserve(imageIds.size());
    for (const auto& id : imageIds)
    {
        images.push_back(WideToMultiByte(id));
    }

    if (context.Args.Contains(ArgType::Output))
    {
        auto& output = context.Args.Get<ArgType::Output>();
        services::ImageService::Save(session, images, output, context.CreateCancelEvent());
    }
    else
    {
        auto stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (wsl::windows::common::wslutil::IsConsoleHandle(stdoutHandle))
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_ImageSaveStdoutIsTerminalError());
        }

        services::ImageService::Save(session, images, stdoutHandle, context.CreateCancelEvent());
    }
}

void TagImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto& source = context.Args.Get<ArgType::Source>();
    auto& target = context.Args.Get<ArgType::Target>();
    services::ImageService::Tag(session, WideToMultiByte(source), WideToMultiByte(target));
}

void PruneImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    bool all = context.Args.Contains(ArgType::All);

    // Filter syntax (`key=value`) is enforced upstream; here we just split on the first '='.
    std::vector<std::pair<std::string, std::string>> filters;
    if (context.Args.Contains(ArgType::Filter))
    {
        for (const auto& wideValue : context.Args.GetAll<ArgType::Filter>())
        {
            std::string raw = WideToMultiByte(wideValue);
            const auto eq = raw.find('=');
            WI_ASSERT(eq != std::string::npos);

            filters.emplace_back(raw.substr(0, eq), raw.substr(eq + 1));
        }
    }

    auto result = ImageService::Prune(session, all, filters);

    for (const auto& image : result.UntaggedImages)
    {
        context.Reporter.Output(L"{}\n", Localization::WSLCCLI_ImagePruneUntagged(image));
    }

    for (const auto& image : result.DeletedImages)
    {
        context.Reporter.Output(L"{}\n", Localization::WSLCCLI_ImagePruneDeleted(image));
    }

    context.Reporter.Output(L"\n");
    context.Reporter.Output(
        L"{}\n", Localization::WSLCCLI_ImagePruneSpaceReclaimedBytes(wsl::shared::string::FormatBytes(result.SpaceReclaimed)));
}
} // namespace wsl::windows::wslc::task
