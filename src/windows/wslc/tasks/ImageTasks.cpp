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
#include "PullImageCallback.h"
#include "TableOutput.h"
#include "Task.h"
#include "TemplateRenderer.h"
#include <format>

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::windows::wslc::core;

namespace wsl::windows::wslc::task {
void BuildImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::Path));
    auto& session = context.Data.Get<Data::Session>();
    auto& contextPath = context.Args.Get<ArgType::Path>();

    auto tags = context.Args.GetAll<ArgType::Tag>();
    auto buildArgs = context.Args.GetAll<ArgType::BuildArg>();

    std::wstring dockerfilePath;
    if (context.Args.Contains(ArgType::File))
    {
        dockerfilePath = context.Args.Get<ArgType::File>();
    }

    PrintMessage(std::format(L"Building image from directory: {}\n", contextPath), stdout);

    WSLCBuildImageFlags flags = WSLCBuildImageFlagsNone;
    WI_SetFlagIf(flags, WSLCBuildImageFlagsVerbose, context.Args.Contains(ArgType::Verbose));
    WI_SetFlagIf(flags, WSLCBuildImageFlagsNoCache, context.Args.Contains(ArgType::NoCache));

    BuildImageCallback callback;
    services::ImageService::Build(session, contextPath, tags, buildArgs, dockerfilePath, flags, &callback, context.CreateCancelEvent());
}

void GetImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto images = ImageService::List(session);
    context.Data.Add<Data::Images>(std::move(images));
}

void ListImages(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Images));
    auto& images = context.Data.Get<Data::Images>();

    if (context.Args.Contains(ArgType::Quiet))
    {
        // Print only the image names.
        for (const auto& image : images)
        {
            PrintMessage(MultiByteToWide(image.Repository.value_or("<untagged>") + ":" + image.Tag.value_or("<untagged>")));
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
        using Config = wsl::windows::wslc::ColumnWidthConfig;
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);

        // Create table — only IMAGE ID uses fixed width; other columns auto-size.
        // When --no-trunc is passed, IMAGE ID also shows full length via TruncateId().
        auto table = trunc ? wsl::windows::wslc::TableOutput<5>(
                                 {{{L"REPOSITORY", {Config::NoLimit, Config::NoLimit, false}},
                                   {L"TAG", {Config::NoLimit, Config::NoLimit, false}},
                                   {L"IMAGE ID", {12, 12, false}},
                                   {L"CREATED", {Config::NoLimit, Config::NoLimit, false}},
                                   {L"SIZE", {Config::NoLimit, Config::NoLimit, false}}}})
                           : wsl::windows::wslc::TableOutput<5>({L"REPOSITORY", L"TAG", L"IMAGE ID", L"CREATED", L"SIZE"});

        for (const auto& image : images)
        {
            table.OutputLine({
                MultiByteToWide(image.Repository.value_or("<untagged>")),
                MultiByteToWide(image.Tag.value_or("<untagged>")),
                MultiByteToWide(TruncateId(image.Id, trunc)),
                ContainerService::FormatRelativeTime(image.Created > 0 ? static_cast<ULONGLONG>(image.Created) : 0),
                std::format(L"{:.2f} MB", static_cast<double>(image.Size) / (1024 * 1024)),
            });
        }

        table.Complete();
        break;
    }
    case FormatType::Template:
    {
        auto templateStr = WideToMultiByte(context.Args.Get<ArgType::Format>());
        for (const auto& image : images)
        {
            auto json = ToJson(image);
            std::wstring result;
            TemplateRenderer::Render(templateStr, json, result);
            PrintMessage(result);
        }
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

    PullImageCallback callback;
    services::ImageService::Pull(session, WideToMultiByte(imageId), &callback);
}

void DeleteImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.Get<ArgType::ImageId>();

    bool force = context.Args.Contains(ArgType::ImageForce);
    bool noPrune = context.Args.Contains(ArgType::NoPrune);
    services::ImageService::Delete(session, WideToMultiByte(imageId), force, noPrune);
}

void LoadImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    if (context.Args.Contains(ArgType::Input))
    {
        auto& input = context.Args.Get<ArgType::Input>();
        services::ImageService::Load(session, input);
        return;
    }

    // TODO Read from stdin if no input argument is provided.
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::WSLCCLI_ImageLoadNoInputError());
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
        auto inspectData = ImageService::Inspect(session, WideToMultiByte(id));
        result.push_back(inspectData);
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    PrintMessage(MultiByteToWide(json));
}

void SaveImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Args.Contains(ArgType::Output));
    auto& session = context.Data.Get<Data::Session>();
    auto& imageId = context.Args.Get<ArgType::ImageId>();
    auto& output = context.Args.Get<ArgType::Output>();
    services::ImageService::Save(session, WideToMultiByte(imageId), output, context.CreateCancelEvent());
}

void TagImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto& source = context.Args.Get<ArgType::Source>();
    auto& target = context.Args.Get<ArgType::Target>();
    services::ImageService::Tag(session, WideToMultiByte(source), WideToMultiByte(target));
}
} // namespace wsl::windows::wslc::task
