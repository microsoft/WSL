/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTasks.cpp

Abstract:

    Implementation of image command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentConvertedTypes.h"
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
        explicit WSLCImageLoadCallback(Terminal& terminal) : m_terminal(terminal)
        {
        }

        HRESULT OnImageLoaded(LPCSTR Reference, EnumReferenceFormat Format) override
        try
        {
            if (Format == EnumReferenceFormatDigest)
            {
                m_terminal.Output(L"{}\n", Localization::WSLCCLI_ImageLoadedId(Reference));
            }
            else if (Format == EnumReferenceFormatTag)
            {
                m_terminal.Output(L"{}\n", Localization::WSLCCLI_ImageLoaded(Reference));
            }
            else
            {
                THROW_HR_MSG(E_UNEXPECTED, "Unexpected reference type: %d, '%hs'", Format, Reference);
            }

            return S_OK;
        }
        CATCH_RETURN();

    private:
        Terminal& m_terminal;
    };

} // namespace

static bool TryInspectImage(Terminal& terminal, Session& session, const std::string& imageId, std::optional<wslc_schema::InspectImage>& inspectData)
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
            terminal.Error(L"{}\n", Localization::MessageWslcImageNotFound(imageId.c_str()));
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
    auto& contextPath = context.Args.GetValue<ArgType::Path>();

    auto tags = context.Args.GetAllValues<ArgType::Tag>();
    auto buildArgs = context.Args.GetAllValues<ArgType::BuildArg>();
    auto labels = context.Args.GetAllValues<ArgType::BuildLabel>();
    auto secrets = context.Args.GetAllValues<ArgType::Secret>();

    std::wstring dockerfilePath;
    if (context.Args.Contains(ArgType::File))
    {
        dockerfilePath = context.Args.GetValue<ArgType::File>();
    }

    std::wstring target;
    if (context.Args.Contains(ArgType::BuildTarget))
    {
        target = context.Args.GetValue<ArgType::BuildTarget>();
    }

    std::optional<services::BuildOutput> output;
    if (context.Args.Contains(ArgType::BuildOutput))
    {
        output = context.Args.GetValue<ArgType::BuildOutput>();
    }

    std::optional<std::wstring> iidFilePath;
    if (context.Args.Contains(ArgType::IidFile))
    {
        iidFilePath = context.Args.GetValue<ArgType::IidFile>();
    }

    WSLCBuildImageFlags flags = WSLCBuildImageFlagsNone;
    WI_SetFlagIf(flags, WSLCBuildImageFlagsVerbose, context.Args.GetValue<ArgType::Verbose>());
    WI_SetFlagIf(flags, WSLCBuildImageFlagsNoCache, context.Args.GetValue<ArgType::NoCache>());
    WI_SetFlagIf(flags, WSLCBuildImageFlagsPull, context.Args.GetValue<ArgType::BuildPull>());

    auto cancelEvent = context.CreateCancelEvent();
    BuildImageCallback callback(context.Terminal, cancelEvent, context.Args.GetValue<ArgType::Verbose>());
    services::ImageService::Build(
        session, contextPath, tags, buildArgs, labels, secrets, dockerfilePath, target, output, iidFilePath, flags, &callback, cancelEvent);
}

void GetImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto images = ImageService::List(session, filters);
    context.Data.Add<Data::Images>(std::move(images));
}

void ListImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Images));
    auto& images = context.Data.Get<Data::Images>();

    if (context.Args.GetValue<ArgType::Quiet>())
    {
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        for (const auto& image : images)
        {
            context.Terminal.Output(L"{}\n", trunc ? TruncateId(image.Id, true) : image.Id);
        }

        return;
    }

    const auto format = context.Args.GetValue<ArgType::Format>(FormatType::Table);

    switch (format)
    {
    case FormatType::Json:
    {
        for (const auto& image : images)
        {
            context.Terminal.Output(L"{}\n", ToJsonW(image, c_jsonCompactIndent));
        }

        break;
    }
    case FormatType::Table:
    {
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        using enum ColumnOverflow;

        // Create table — only IMAGE ID uses fixed width; other columns shrink to fit the console.
        // When --no-trunc is passed, IMAGE ID also shows full length via TruncateId().
        auto table =
            trunc
                ? wsl::windows::wslc::TableOutput<5>(
                      context.Terminal,
                      {{{L"REPOSITORY", {.Overflow = Shrink}},
                        {L"TAG", {.Overflow = Shrink}},
                        {L"IMAGE ID", {.MinWidth = 12, .MaxWidth = 12, .Overflow = Shrink}},
                        {L"CREATED", {.Overflow = Shrink}},
                        {L"SIZE", {.Overflow = Shrink}}}},
                      images.size())
                : wsl::windows::wslc::TableOutput<5>(context.Terminal, {L"REPOSITORY", L"TAG", L"IMAGE ID", L"CREATED", L"SIZE"});

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
    const auto image = WideToMultiByte(context.Args.GetValue<ArgType::ImageId>());
    const bool quiet = context.Args.GetValue<ArgType::Quiet>();

    // Match `docker pull`: for a name-only reference (no tag or digest) the tag defaults to "latest". Unless quiet,
    // the client reports this on stdout before contacting the registry.
    const auto reference = ImageReference::Parse(image);
    if (!quiet && reference.Format == EnumReferenceFormatNone)
    {
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_PullUsingDefaultTag(L"latest"));
    }

    // Match `docker pull`: in quiet mode, suppress progress output by passing no progress callback. Warnings are
    // unaffected because the warning callback is built internally by ImageService::Pull from the Terminal.
    std::optional<ImageProgressCallback> callback;
    if (!quiet)
    {
        callback.emplace(context.Terminal, Terminal::Level::Output);
    }

    IProgressCallback* progress = callback ? &*callback : nullptr;
    services::ImageService::Pull(context.Terminal, session, image, progress);

    // Match `docker pull`: always print the resolved canonical image reference as the final line.
    context.Terminal.Output(L"{}\n", MultiByteToWide(reference.GetCanonical()));
}

void PushImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.GetValue<ArgType::ImageId>();

    ImageProgressCallback callback(context.Terminal, Terminal::Level::Output);
    services::ImageService::Push(context.Terminal, session, WideToMultiByte(imageId), &callback);
}

void DeleteImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto imageIds = context.Args.GetAllValues<ArgType::ImageId>();
    bool force = context.Args.GetValue<ArgType::ImageForce>();
    bool noPrune = context.Args.GetValue<ArgType::NoPrune>();
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
        auto& input = context.Args.GetValue<ArgType::Input>();
        auto callback = wil::MakeOrThrow<WSLCImageLoadCallback>(context.Terminal);
        services::ImageService::Load(context.Terminal, session, input, callback.Get());
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
        imageName = WideToMultiByte(context.Args.GetValue<ArgType::ImageId>());
    }

    auto& input = context.Args.GetValue<ArgType::ImportFile>();
    auto imageId = services::ImageService::Import(context.Terminal, session, input, imageName);
    if (!imageId.empty())
    {
        bool trunc = !context.Args.GetValue<ArgType::NoTrunc>();
        context.Terminal.Output(L"{}\n", MultiByteToWide(TruncateId(imageId, trunc)));
    }
}

void InspectImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto imageIds = context.Args.GetAllValues<ArgType::ImageId>();

    std::vector<wsl::windows::common::wslc_schema::InspectImage> result;
    for (const auto& id : imageIds)
    {
        std::optional<wslc_schema::InspectImage> inspectData;
        if (TryInspectImage(context.Terminal, session, WideToMultiByte(id), inspectData))
        {
            result.push_back(*inspectData);
        }
        else
        {
            context.ExitCode = 1;
        }
    }

    auto json = ToJson(result, context.Args.GetValue<ArgType::InspectFormat>(c_jsonPrettyPrintIndent));
    context.Terminal.Output(L"{}\n", MultiByteToWide(json));
}

void SaveImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto imageIds = context.Args.GetAllValues<ArgType::ImageId>();

    std::vector<std::string> images;
    images.reserve(imageIds.size());
    for (const auto& id : imageIds)
    {
        images.push_back(WideToMultiByte(id));
    }

    if (context.Args.Contains(ArgType::Output))
    {
        auto& output = context.Args.GetValue<ArgType::Output>();
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
    auto& source = context.Args.GetValue<ArgType::Source>();
    auto& target = context.Args.GetValue<ArgType::Target>();
    services::ImageService::Tag(session, WideToMultiByte(source), WideToMultiByte(target));
}

void PruneImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    bool all = context.Args.GetValue<ArgType::All>();

    // Filter values are parsed and cached during argument validation.
    auto filters = context.Args.GetAllValues<ArgType::Filter>();

    auto result = ImageService::Prune(session, all, filters);

    for (const auto& image : result.UntaggedImages)
    {
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_ImagePruneUntagged(image));
    }

    for (const auto& image : result.DeletedImages)
    {
        context.Terminal.Output(L"{}\n", Localization::WSLCCLI_ImagePruneDeleted(image));
    }

    context.Terminal.Output(L"\n");
    context.Terminal.Output(
        L"{}\n", Localization::WSLCCLI_ImagePruneSpaceReclaimedBytes(wsl::shared::string::FormatBytes(result.SpaceReclaimed)));
}
} // namespace wsl::windows::wslc::task
