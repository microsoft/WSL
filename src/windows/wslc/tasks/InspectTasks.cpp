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
#include "NetworkService.h"
#include "VolumeService.h"
#include "ContainerService.h"
#include "ExecutionContext.h"

namespace wsl::windows::wslc::task {

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::models;

// Detects podman's name-validation error messages that arrive as E_FAIL
// (because podman's docker-compat API returns 5xx instead of 400 Bad Request
// for syntactically invalid references such as uppercase image names).
// Semantically these mean "the name is not a valid reference, therefore it
// does not refer to any object of this type" — equivalent to not-found for
// inspect's lenient lookup. Returning true here lets TryInspect tolerate
// the error and continue with the next type lookup.
static bool IsPodmanNameValidationError(const std::wstring& message)
{
    return message.find(L"parsing reference") != std::wstring::npos || message.find(L"normalizing name") != std::wstring::npos ||
           message.find(L"must be lowercase") != std::wstring::npos;
}

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

        // podman-compat quirk: name-validation failures come back as 5xx
        // (mapped to E_FAIL) instead of 400 Bad Request. Treat as not-found
        // so root `inspect` retains its docker-era lenient behavior — any
        // invalid name should yield "Object not found", not a raw E_FAIL.
        //
        // The detailed error text from podman doesn't survive on
        // wil::ResultException::what() (which carries source-location
        // info) or on the TLS IErrorInfo (already consumed by wil when
        // CollectError ran during the throw). It is captured in the
        // current ExecutionContext's reported error, so pattern-match
        // there.
        if (errorCode == E_FAIL)
        {
            if (auto* context = wsl::windows::common::ExecutionContext::Current())
            {
                const auto& reported = context->ReportedError();
                if (reported.has_value() && reported->Message.has_value() && IsPodmanNameValidationError(*reported->Message))
                {
                    LOG_HR_MSG(errorCode, "Tolerating podman name-validation error in inspect");
                    return false;
                }
            }
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
    return TryInspect([&]() { result = services::ContainerService::Inspect(session, containerId); }, WSLC_E_CONTAINER_NOT_FOUND);
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
    auto objectIds = context.Args.GetAll<ArgType::ObjectId>();

    nlohmann::json array = nlohmann::json::array();
    auto type = InspectType::All;
    if (context.Args.Contains(ArgType::Type))
    {
        type = validation::GetInspectTypeFromString(context.Args.Get<ArgType::Type>(), L"type");
    }

    for (const auto& objectId : objectIds)
    {
        auto id = WideToMultiByte(objectId);
        std::optional<wslc_schema::InspectContainer> container;
        std::optional<wslc_schema::InspectImage> image;
        std::optional<wslc_schema::Network> network;
        std::optional<wslc_schema::InspectVolume> volume;

        if (WI_IsFlagSet(type, InspectType::Container) && TryInspectContainer(session, id, container))
        {
            array.push_back(std::move(*container));
        }
        else if (WI_IsFlagSet(type, InspectType::Image) && TryInspectImage(session, id, image))
        {
            array.push_back(std::move(*image));
        }
        else if (WI_IsFlagSet(type, InspectType::Network) && TryInspectNetwork(session, id, network))
        {
            array.push_back(std::move(*network));
        }
        else if (WI_IsFlagSet(type, InspectType::Volume) && TryInspectVolume(session, id, volume))
        {
            array.push_back(std::move(*volume));
        }
        else
        {
            PrintMessage(Localization::WSLCCLI_ObjectNotFoundError(objectId), stderr);
            context.ExitCode = 1;
        }
    }

    // Always print the array, even if it's empty or an error was encountered
    PrintMessage(MultiByteToWide(array.dump(c_jsonPrettyPrintIndent)));
}
} // namespace wsl::windows::wslc::task
