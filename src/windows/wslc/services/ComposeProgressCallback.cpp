// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeProgressCallback.h"
#include <relay.hpp>

namespace wsl::windows::wslc::services {

namespace {

    std::wstring ToWide(LPCSTR value)
    {
        return value == nullptr ? std::wstring{} : wsl::shared::string::MultiByteToWide(value);
    }

    std::wstring ComposeOperationText(LPCSTR operation)
    {
        if (operation != nullptr)
        {
            const std::string_view operationValue{operation};
            if (operationValue == "create")
            {
                return wsl::shared::Localization::WSLCCLI_ComposeProgressCreating();
            }
            if (operationValue == "remove")
            {
                return wsl::shared::Localization::WSLCCLI_ComposeProgressRemoving();
            }
            if (operationValue == "pull")
            {
                return wsl::shared::Localization::WSLCCLI_ComposeProgressPulling();
            }
            if (operationValue == "start")
            {
                return wsl::shared::Localization::WSLCCLI_ComposeProgressStarting();
            }
            if (operationValue == "stop")
            {
                return wsl::shared::Localization::WSLCCLI_ComposeProgressStopping();
            }
        }

        return operation == nullptr ? std::wstring{} : wsl::shared::string::MultiByteToWide(operation);
    }

    std::wstring ComposeResourceText(LPCSTR unit, LPCSTR resourceKey)
    {
        std::wstring resourceType;
        if (unit != nullptr)
        {
            const std::string_view unitValue{unit};
            if (unitValue == "container")
            {
                resourceType = wsl::shared::Localization::WSLCCLI_ComposeResourceContainer();
            }
            else if (unitValue == "network")
            {
                resourceType = wsl::shared::Localization::WSLCCLI_ComposeResourceNetwork();
            }
            else if (unitValue == "image")
            {
                resourceType = wsl::shared::Localization::WSLCCLI_ComposeResourceImage();
            }
        }

        const auto resourceKeyValue = ToWide(resourceKey);
        return resourceType.empty() ? resourceKeyValue : wsl::shared::Localization::MessageWslcComposeResource(resourceType, resourceKeyValue);
    }

} // namespace

HRESULT ComposeProgressCallback::OnProgress(const WSLCComposeProgressEvent* event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, event);
    RETURN_HR_IF(E_INVALIDARG, event->SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);

    switch (event->Kind)
    {
    case WSLCComposeProgressEventKindStatus:
        if (m_action == WSLCComposeActionRemove && event->Value.Status.Status == WSLCComposeStatusSucceeded && !m_removeProgressReported)
        {
            m_terminal.Info(L"{}\n", wsl::shared::Localization::WSLCCLI_ComposeNoStoppedContainers());
        }
        break;

    case WSLCComposeProgressEventKindProgress:
        if (m_action == WSLCComposeActionRemove && event->Value.Progress.Operation != nullptr &&
            std::string_view{event->Value.Progress.Operation} == "remove")
        {
            m_removeProgressReported = true;
        }

        m_terminal.Info(
            L"{}\n",
            wsl::shared::Localization::MessageWslcComposeProgress(
                ComposeOperationText(event->Value.Progress.Operation),
                ComposeResourceText(event->Value.Progress.Unit, event->Value.Progress.ResourceKey),
                event->Value.Progress.Current,
                event->Value.Progress.Total));
        break;

    case WSLCComposeProgressEventKindDiagnostic:
    {
        const auto code = ToWide(event->Value.Diagnostic.Code);
        switch (event->Value.Diagnostic.Severity)
        {
        case WSLCComposeDiagnosticSeverityInfo:
            m_terminal.Info(L"{}\n", code);
            break;
        case WSLCComposeDiagnosticSeverityWarning:
            m_terminal.Warn(L"{}\n", code);
            break;
        case WSLCComposeDiagnosticSeverityError:
            m_terminal.Error(L"{}\n", code);
            break;
        default:
            THROW_HR(E_INVALIDARG);
        }
        break;
    }

    default:
        THROW_HR(E_INVALIDARG);
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT ComposeProgressCallback::OnStreamsReady(const WSLCComposeStreams* streams)
try
{
    RETURN_HR_IF_NULL(E_POINTER, streams);

    common::io::MultiHandleWait io;
    if (streams->Stdout.Type != WSLCHandleTypeUnknown)
    {
        io.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            wil::unique_handle{streams->Stdout.Handle.File}, GetStdHandle(STD_OUTPUT_HANDLE)));
    }

    if (streams->Stderr.Type != WSLCHandleTypeUnknown)
    {
        io.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            wil::unique_handle{streams->Stderr.Handle.File}, GetStdHandle(STD_ERROR_HANDLE)));
    }

    wil::unique_handle stdinHandle;
    if (streams->Stdin.Type != WSLCHandleTypeUnknown)
    {
        stdinHandle.reset(streams->Stdin.Handle.File);
    }

    io.Run({});
    return S_OK;
}
CATCH_RETURN();

} // namespace wsl::windows::wslc::services
