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
    std::wstring id;
    std::wstring type;
    std::wstring envName;
    std::wstring srcPath;

    for (const auto& part : wsl::shared::string::Split(spec, L','))
    {
        auto eq = part.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG, Localization::MessageWslcSecretInvalidSpec(spec, L"expected key=value pairs separated by ','"));
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
        else
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unsupported key '{}'", key)));
        }
    }

    if (id.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcSecretInvalidSpec(spec, L"'id=' is required"));
    }

    // The id is forwarded into docker's comma/'='-delimited --secret spec, so reject any character
    // that could break out of the id= field and inject additional options (e.g. ",src=/etc/passwd").
    for (auto ch : id)
    {
        const bool allowed = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                             ch == L'_' || ch == L'-' || ch == L'.';
        if (!allowed)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG,
                Localization::MessageWslcSecretInvalidSpec(spec, L"'id' may only contain letters, digits, '_', '-' or '.'"));
        }
    }

    if (!type.empty() && type != L"file" && type != L"env")
    {
        THROW_HR_WITH_USER_ERROR(
            E_INVALIDARG, Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unsupported secret type '{}'", type)));
    }

    // Docker parity: 'type=file' names a source file, so it requires 'src='. Without it we would
    // otherwise fall through to reading an environment variable, silently contradicting the type.
    if (type == L"file" && srcPath.empty())
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcSecretInvalidSpec(spec, L"'type=file' requires 'src='"));
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
        // environment variable whose name matches the id. Unlike an explicit 'env=', that variable
        // must be set - Docker errors when the id-named variable is undefined.
        envName = id;
        if (GetEnvironmentVariableW(envName.c_str(), nullptr, 0) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG,
                Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"environment variable '{}' is not set", envName)));
        }
    }

    if (!srcPath.empty())
    {
        std::error_code ec;
        // Resolve symlinks (and normalize '..') so we read the file that actually holds the secret's
        // bytes rather than the link node itself.
        auto absPath = std::filesystem::weakly_canonical(std::filesystem::absolute(srcPath), ec);
        if (ec.value() != 0 || !std::filesystem::is_regular_file(absPath, ec))
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG,
                Localization::MessageWslcSecretInvalidSpec(
                    spec, std::format(L"source file not found or not a regular file: {}", absPath.wstring())));
        }

        // Read the file's raw bytes and forward them verbatim. The server materializes them into a
        // root-only tmpfs file inside the VM, so file secrets are byte-exact (binary, embedded NULs,
        // and arbitrary size all round-trip) - matching Docker's type=file semantics - without ever
        // mounting a host directory into the VM.
        std::ifstream file(absPath, std::ios::binary);
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcSecretInvalidSpec(spec, std::format(L"unable to open source file: {}", absPath.wstring())),
            !file);
        return services::BuildSecret{
            .Id = std::move(id),
            .Value = std::vector<BYTE>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()),
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

static bool TryInspectImage(Session& session, const std::string& imageId, std::optional<wslc_schema::InspectImage>& inspectData)
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
            PrintMessage(Localization::MessageWslcImageNotFound(imageId.c_str()), stderr);
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

    PrintMessage(std::format(L"Building image from directory: {}\n", contextPath), stdout);

    WSLCBuildImageFlags flags = WSLCBuildImageFlagsNone;
    WI_SetFlagIf(flags, WSLCBuildImageFlagsVerbose, context.Args.Contains(ArgType::Verbose));
    WI_SetFlagIf(flags, WSLCBuildImageFlagsNoCache, context.Args.Contains(ArgType::NoCache));
    WI_SetFlagIf(flags, WSLCBuildImageFlagsPull, context.Args.Contains(ArgType::BuildPull));

    auto cancelEvent = context.CreateCancelEvent();
    BuildImageCallback callback(cancelEvent, context.Args.Contains(ArgType::Verbose));
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
        PrintMessage(MultiByteToWide(json));
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

    ImageProgressCallback callback;
    services::ImageService::Pull(session, WideToMultiByte(imageId), &callback);
}

void PushImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.Get<ArgType::ImageId>();

    ImageProgressCallback callback;
    services::ImageService::Push(session, WideToMultiByte(imageId), &callback);
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
        services::ImageService::Load(session, input, callback.Get());
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
    auto imageId = services::ImageService::Import(session, input, imageName);
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
        if (TryInspectImage(session, WideToMultiByte(id), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    PrintMessage(MultiByteToWide(json));
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
        PrintMessage(Localization::WSLCCLI_ImagePruneUntagged(image));
    }

    for (const auto& image : result.DeletedImages)
    {
        PrintMessage(Localization::WSLCCLI_ImagePruneDeleted(image));
    }

    PrintMessage(L"");
    PrintMessage(Localization::WSLCCLI_ImagePruneSpaceReclaimedBytes(wsl::shared::string::FormatBytes(result.SpaceReclaimed)));
}
} // namespace wsl::windows::wslc::task
