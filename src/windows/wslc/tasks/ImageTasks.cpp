/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTasks.cpp

Abstract:

    Implementation of image command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "ImageModel.h"
#include "ImageService.h"
#include "ImageTasks.h"
#include "PullImageCallback.h"
#include "TablePrinter.h"
#include "Task.h"
#include <format>

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {
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
            PrintMessage(MultiByteToWide(image.Name));
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
        auto json = ToJson(images);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        utils::TablePrinter tablePrinter({L"NAME", L"SIZE (MB)"});
        for (const auto& image : images)
        {
            tablePrinter.AddRow({MultiByteToWide(image.Name), std::format(L"{:.2f}", static_cast<double>(image.Size) / (1024 * 1024))});
        }

        tablePrinter.Print();
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

void LoadImage(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();

    if (context.Args.Contains(ArgType::Input))
    {
        auto& input = context.Args.Get<ArgType::Input>();
        services::ImageService::Load(session, WideToMultiByte(input));
        return;
    }

    // TODO Read from stdin if no input argument is provided.
    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, L"Requested load but no input provided.");
}
} // namespace wsl::windows::wslc::task
