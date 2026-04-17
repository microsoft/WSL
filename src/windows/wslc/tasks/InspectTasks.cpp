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

template <typename TInspectFn>
static bool TryInspect(TInspectFn&& fn, HRESULT notFoundError)
{
    try
    {
        fn();
        return true;
    }
    catch (const wil::ResultException& ex)
    {
        if (ex.GetErrorCode() == notFoundError || ex.GetErrorCode() == HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS))
        {
            return false;
        }

        throw;
    }
}

static bool TryInspectImage(wsl::windows::wslc::models::Session& session, const std::string& image, std::optional<wslc_schema::InspectImage>& result)
{
    return TryInspect([&]() { result = services::ImageService::Inspect(session, image); }, WSLC_E_IMAGE_NOT_FOUND);
}

static bool TryInspectContainer(wsl::windows::wslc::models::Session& session, const std::string& containerId, std::optional<wslc_schema::InspectContainer>& result)
{
    return TryInspect([&]() { result = services::ContainerService::Inspect(session, containerId); }, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
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
        std::optional<wslc_schema::InspectContainer> container;
        std::optional<wslc_schema::InspectImage> image;

        if (WI_IsFlagSet(type, InspectType::Container) && TryInspectContainer(session, id, container))
        {
            array.push_back(std::move(*container));
        }
        else if (WI_IsFlagSet(type, InspectType::Image) && TryInspectImage(session, id, image))
        {
            array.push_back(std::move(*image));
        }
        else
        {
            PrintMessage(std::format(L"Object not found: {}", objectId), stderr);
        }
    }

    PrintMessage(MultiByteToWide(array.dump(c_jsonPrettyPrintIndent)));
}
} // namespace wsl::windows::wslc::task
