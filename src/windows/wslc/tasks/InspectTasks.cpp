/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectTasks.cpp

Abstract:

    Implementation of inspection command related execution logic.
--*/

#include "Argument.h"
#include "ArgumentValidation.h"
#include "InspectTasks.h"
#include "InspectModel.h"
#include "ImageService.h"
#include "ContainerService.h"

namespace wsl::windows::wslc::task {

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::models;

static std::optional<wslc_schema::InspectImage> TryInspectImage(wsl::windows::wslc::models::Session& session, const std::string& image)
{
    try
    {
        return services::ImageService::Inspect(session, image);
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == WSLC_E_IMAGE_NOT_FOUND)
        {
            return std::nullopt;
        }

        throw;
    }
}

static std::optional<wslc_schema::InspectContainer> TryInspectContainer(wsl::windows::wslc::models::Session& session, const std::string& containerId)
{
    try
    {
        return services::ContainerService::Inspect(session, containerId);
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            return std::nullopt;
        }
        throw;
    }
}

void Inspect(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ObjectId));
    auto& session = context.Data.Get<Data::Session>();
    auto objectIds = context.Args.GetAll<ArgType::ObjectId>();

    nlohmann::json array = nlohmann::json::array();
    auto type = InspectType::All;
    if (context.Args.Contains(ArgType::Type))
    {
        type = validation::GetInspectTypeFromString(context.Args.Get<ArgType::Type>());
    }

    for (const auto& objectId : objectIds)
    {
        auto id = WideToMultiByte(objectId);

        // 1. Try to inspect object as container
        if (WI_IsFlagSet(type, InspectType::Container))
        {
            auto container = TryInspectContainer(session, id);
            if (container)
            {
                array.push_back(*container);
                continue;
            }
        }

        // 2. Try to inspect object as image
        if (WI_IsFlagSet(type, InspectType::Image))
        {
            auto image = TryInspectImage(session, id);
            if (image)
            {
                array.push_back(*image);
                continue;
            }
        }
    }

    // Check if array is empty then throw error not found
    if (array.empty())
    {
        THROW_HR(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    PrintMessage(MultiByteToWide(array.dump()));
}
} // namespace wsl::windows::wslc::task
