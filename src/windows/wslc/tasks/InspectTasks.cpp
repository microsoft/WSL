/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectTasks.cpp

Abstract:

    Implementation of inspection command related execution logic.
--*/

#include "Argument.h"
#include "ArgumentConvertedTypes.h"
#include "InspectTasks.h"
#include "InspectModel.h"
#include "ImageService.h"
#include "NetworkService.h"
#include "VolumeService.h"
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
        auto errorCode = ex.GetErrorCode();
        if (errorCode == notFoundError || errorCode == HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS) || errorCode == E_INVALIDARG)
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

static bool TryInspectContainer(
    wsl::windows::wslc::models::Session& session, const std::string& containerId, std::optional<wslc_schema::InspectContainer>& result, bool size)
{
    return TryInspect([&]() { result = services::ContainerService::Inspect(session, containerId, size); }, WSLC_E_CONTAINER_NOT_FOUND);
}

static bool TryInspectNetwork(wsl::windows::wslc::models::Session& session, const std::string& networkName, std::optional<wslc_schema::Network>& result)
{
    return TryInspect([&]() { result = services::NetworkService::Inspect(session, networkName); }, WSLC_E_NETWORK_NOT_FOUND);
}

static bool TryInspectVolume(wsl::windows::wslc::models::Session& session, const std::string& volumeId, std::optional<wslc_schema::InspectVolume>& result)
{
    return TryInspect([&]() { result = services::VolumeService::Inspect(session, volumeId); }, WSLC_E_VOLUME_NOT_FOUND);
}

void Inspect(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto objectIds = context.Args.GetAllValues<ArgType::ObjectId>();

    nlohmann::json array = nlohmann::json::array();
    auto type = InspectType::All;
    if (context.Args.Contains(ArgType::Type))
    {
        type = context.Args.GetValue<ArgType::Type>();
    }

    const bool size = context.Args.GetValue<ArgType::Size>();

    // Only containers carry file size information; every other type warns and continues.
    const auto warnSizeIgnored = [&](const wchar_t* objectType) {
        if (size)
        {
            context.Terminal.Error(L"{}\n", Localization::WSLCCLI_InspectSizeIgnoredWarning(objectType));
        }
    };

    for (const auto& objectId : objectIds)
    {
        auto id = WideToMultiByte(objectId);
        std::optional<wslc_schema::InspectContainer> container;
        std::optional<wslc_schema::InspectImage> image;
        std::optional<wslc_schema::Network> network;
        std::optional<wslc_schema::InspectVolume> volume;

        if (WI_IsFlagSet(type, InspectType::Container) && TryInspectContainer(session, id, container, size))
        {
            array.push_back(wslc_schema::ToInspectJson(*container));
        }
        else if (WI_IsFlagSet(type, InspectType::Image) && TryInspectImage(session, id, image))
        {
            warnSizeIgnored(L"image");
            array.push_back(std::move(*image));
        }
        else if (WI_IsFlagSet(type, InspectType::Network) && TryInspectNetwork(session, id, network))
        {
            warnSizeIgnored(L"network");
            array.push_back(std::move(*network));
        }
        else if (WI_IsFlagSet(type, InspectType::Volume) && TryInspectVolume(session, id, volume))
        {
            warnSizeIgnored(L"volume");
            array.push_back(std::move(*volume));
        }
        else
        {
            context.Terminal.Error(L"{}\n", Localization::WSLCCLI_ObjectNotFoundError(objectId));
            context.ExitCode = 1;
        }
    }

    // Always print the array, even if it's empty or an error was encountered
    context.Terminal.Output(L"{}\n", MultiByteToWide(array.dump(context.Args.GetValue<ArgType::InspectFormat>(c_jsonPrettyPrintIndent))));
}
} // namespace wsl::windows::wslc::task
