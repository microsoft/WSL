/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionFactory.cpp

Abstract:

    Implementation for WSLASessionFactory.

    Creates WSLASession objects in the per-user COM server process along with
    their corresponding IWSLASessionReference weak references for the SYSTEM
    service to track session lifetime.

--*/

#include "WSLASessionFactory.h"
#include "WSLASession.h"
#include "WSLASessionReference.h"
#include "wslutil.h"

namespace wslutil = wsl::windows::common::wslutil;
namespace wsla = wsl::windows::service::wsla;

void wsla::WSLASessionFactory::SetDestructionCallback(std::function<void()> callback)
{
    m_destructionCallback = std::move(callback);
}

HRESULT wsla::WSLASessionFactory::CreateSession(
    _In_ const WSLA_SESSION_INIT_SETTINGS* Settings, _In_ IWSLAVirtualMachine* Vm, _Out_ IWSLASession** Session, _Out_ IWSLASessionReference** ServiceRef)
try
{
    *Session = nullptr;
    *ServiceRef = nullptr;

    // Create the session object.
    auto session = Microsoft::WRL::Make<wsla::WSLASession>();

    // Pass the destruction callback directly to the session.
    // One session per process, so when it's destroyed, exit.
    session->SetDestructionCallback(m_destructionCallback);

    // Initialize the session with the VM.
    RETURN_IF_FAILED(session->Initialize(Settings, Vm));

    // Create the service session ref. It extracts metadata and a weak reference from the session.
    auto serviceRef = Microsoft::WRL::Make<wsla::WSLASessionReference>(session.Get());

    // Return the session as IWSLASession interface
    RETURN_IF_FAILED(session->QueryInterface(IID_PPV_ARGS(Session)));
    *ServiceRef = serviceRef.Detach();

    WSL_LOG(
        "WSLASessionFactoryCreatedSession",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(Settings->SessionId, "SessionId"),
        TraceLoggingWideString(Settings->DisplayName, "DisplayName"));

    return S_OK;
}
CATCH_RETURN()

HRESULT wsla::WSLASessionFactory::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    wil::unique_handle process{OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId())};
    RETURN_LAST_ERROR_IF(!process);

    *ProcessHandle = process.release();
    return S_OK;
}
CATCH_RETURN()
