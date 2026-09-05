// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeProgressCallback.h"
#include <relay.hpp>

namespace wsl::windows::wslc::services {

namespace {

    // Placeholder rendering keeps typed progress visible for engineering and demonstrations until the Compose progress UI is defined.
    std::wstring ComposeStatusText(WSLCComposeStatus Status)
    {
        switch (Status)
        {
        case WSLCComposeStatusValidating:
            return L"Validating Compose project";
        case WSLCComposeStatusPlanning:
            return L"Planning Compose operation";
        case WSLCComposeStatusExecuting:
            return L"Executing Compose operation";
        case WSLCComposeStatusSucceeded:
            return L"Compose operation succeeded";
        case WSLCComposeStatusFailed:
            return L"Compose operation failed";
        case WSLCComposeStatusCancelled:
            return L"Compose operation cancelled";
        default:
            THROW_HR(E_INVALIDARG);
        }
    }

    std::wstring ToWide(LPCSTR Value)
    {
        return Value == nullptr ? std::wstring{} : wsl::shared::string::MultiByteToWide(Value);
    }

} // namespace

HRESULT ComposeProgressCallback::OnProgress(const WSLCComposeProgressEvent* Event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Event);
    RETURN_HR_IF(E_INVALIDARG, Event->SchemaVersion != WSLC_COMPOSE_SCHEMA_VERSION);

    switch (Event->Kind)
    {
    case WSLCComposeProgressEventKindStatus:
        m_terminal.Output(L"{}\n", ComposeStatusText(Event->Value.Status.Status));
        break;

    case WSLCComposeProgressEventKindProgress:
        m_terminal.Info(
            L"{}: {} / {} {}\n",
            ToWide(Event->Value.Progress.ResourceKey),
            Event->Value.Progress.Current,
            Event->Value.Progress.Total,
            ToWide(Event->Value.Progress.Unit));
        break;

    case WSLCComposeProgressEventKindDiagnostic:
    {
        const auto code = ToWide(Event->Value.Diagnostic.Code);
        switch (Event->Value.Diagnostic.Severity)
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

HRESULT ComposeProgressCallback::OnStreamsReady(const WSLCComposeStreams* Streams)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Streams);

    common::io::MultiHandleWait io;
    if (Streams->Stdout.Type != WSLCHandleTypeUnknown)
    {
        io.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            wil::unique_handle{Streams->Stdout.Handle.File}, GetStdHandle(STD_OUTPUT_HANDLE)));
    }

    if (Streams->Stderr.Type != WSLCHandleTypeUnknown)
    {
        io.AddHandle(std::make_unique<common::io::RelayHandle<common::io::ReadHandle>>(
            wil::unique_handle{Streams->Stderr.Handle.File}, GetStdHandle(STD_ERROR_HANDLE)));
    }

    wil::unique_handle stdinHandle;
    if (Streams->Stdin.Type != WSLCHandleTypeUnknown)
    {
        stdinHandle.reset(Streams->Stdin.Handle.File);
    }

    io.Run({});
    return S_OK;
}
CATCH_RETURN();

} // namespace wsl::windows::wslc::services
