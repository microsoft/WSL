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

    common::io::HandleWrapper DuplicateComposeStreamHandle(const WSLCHandle& handle)
    {
        // Callback input handles expire on return; the relay retains owned duplicates.
        switch (handle.Type)
        {
        case WSLCHandleTypeFile:
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), handle.Handle.File == nullptr || handle.Handle.File == INVALID_HANDLE_VALUE);
            return common::io::HandleWrapper{wil::unique_handle{common::wslutil::DuplicateHandle(handle.Handle.File, std::nullopt, FALSE)}};

        case WSLCHandleTypePipe:
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), handle.Handle.Pipe == nullptr || handle.Handle.Pipe == INVALID_HANDLE_VALUE);
            return common::io::HandleWrapper{wil::unique_handle{common::wslutil::DuplicateHandle(handle.Handle.Pipe, std::nullopt, FALSE)}};

        case WSLCHandleTypeSocket:
        {
            const auto socket = reinterpret_cast<SOCKET>(handle.Handle.Socket);
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), socket == INVALID_SOCKET);
            WSAPROTOCOL_INFOW protocolInfo{};
            const auto result = WSADuplicateSocketW(socket, GetCurrentProcessId(), &protocolInfo);
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result == SOCKET_ERROR);
            wil::unique_socket duplicate{WSASocketW(
                FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocolInfo, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)};
            THROW_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), !duplicate);
            return common::io::HandleWrapper{std::move(duplicate)};
        }

        case WSLCHandleTypeUnknown:
        default:
            THROW_HR_MSG(E_INVALIDARG, "Unsupported Compose stream handle type: %d", handle.Type);
        }
    }

} // namespace

HRESULT ComposeProgressCallback::OnProgress(const WSLCComposeProgressEvent* event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, event);

    if (event->Progress != nullptr)
    {
        const auto& progress = *event->Progress;
        if (m_action == WSLCComposeActionRemove && progress.Operation != nullptr &&
            std::string_view{progress.Operation} == "remove")
        {
            m_removeProgressReported = true;
        }

        m_terminal.Info(
            L"{}\n",
            wsl::shared::Localization::MessageWslcComposeProgress(
                ComposeOperationText(progress.Operation),
                ComposeResourceText(progress.Unit, progress.ResourceKey),
                progress.Current,
                progress.Total));
    }

    if (event->Diagnostic != nullptr)
    {
        const auto& diagnostic = *event->Diagnostic;
        const auto code = ToWide(diagnostic.Code);
        switch (diagnostic.Severity)
        {
        case WSLCComposeDiagnosticSeverityInfo:
            m_terminal.Info(L"{}\n", code);
            break;
        case WSLCComposeDiagnosticSeverityWarning:
            m_terminal.Warn(L"{}\n", code);
            break;
        case WSLCComposeDiagnosticSeverityError:
            // The command's failure renderer consumes operation_failed through the final result.
            if (diagnostic.Code != nullptr && std::string_view{diagnostic.Code} == WSLC_COMPOSE_DIAGNOSTIC_OPERATION_FAILED)
            {
                break;
            }
            m_terminal.Error(L"{}\n", code);
            break;
        default:
            THROW_HR(E_INVALIDARG);
        }
    }

    if (m_action == WSLCComposeActionRemove && event->Status == WSLCComposeStatusSucceeded && !m_removeProgressReported)
    {
        m_terminal.Info(L"{}\n", wsl::shared::Localization::WSLCCLI_ComposeNoStoppedContainers());
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT ComposeProgressCallback::OnStreamsReady(const WSLCComposeStreams* streams)
try
{
    RETURN_HR_IF_NULL(E_POINTER, streams);

    if (streams->Stdout.Type != WSLCHandleTypeUnknown)
    {
        m_streamIo.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            DuplicateComposeStreamHandle(streams->Stdout), GetStdHandle(STD_OUTPUT_HANDLE)));
    }

    if (streams->Stderr.Type != WSLCHandleTypeUnknown)
    {
        m_streamIo.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            DuplicateComposeStreamHandle(streams->Stderr), GetStdHandle(STD_ERROR_HANDLE)));
    }

    return S_OK;
}
CATCH_RETURN();

void ComposeProgressCallback::RelayStreams(HANDLE cancelEvent)
{
    if (cancelEvent != nullptr)
    {
        m_streamIo.AddHandle(
            std::make_unique<common::io::EventHandle>(cancelEvent),
            common::io::MultiHandleWait::CancelOnCompleted | common::io::MultiHandleWait::NeedNotComplete);
    }

    m_streamIo.Run({});
}

} // namespace wsl::windows::wslc::services
