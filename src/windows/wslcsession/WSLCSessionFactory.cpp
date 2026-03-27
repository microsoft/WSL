/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionFactory.cpp

Abstract:

    Implementation for WSLCSessionFactory.

    Creates WSLCSession objects in the per-user COM server process along with
    their corresponding IWSLCSessionReference weak references for the SYSTEM
    service to track session lifetime.

--*/

#include "WSLCSessionFactory.h"
#include "WSLCSession.h"
#include "WSLCSessionReference.h"
#include "wslutil.h"

namespace wslutil = wsl::windows::common::wslutil;
namespace wslc = wsl::windows::service::wslc;

void wslc::WSLCSessionFactory::SetDestructionCallback(std::function<void()>&& callback)
{
    m_destructionCallback = std::move(callback);
}

HRESULT wslc::WSLCSessionFactory::CreateSession(
    _In_ const WSLCSessionInitSettings* Settings, _In_ IWSLCVirtualMachine* Vm, _Out_ IWSLCSession** Session, _Out_ IWSLCSessionReference** ServiceRef)
try
{
    *Session = nullptr;
    *ServiceRef = nullptr;

    // Create the session object.
    auto session = Microsoft::WRL::Make<wslc::WSLCSession>();

    // Pass the destruction callback directly to the session.
    // One session per process, so when it's destroyed, exit.
    session->SetDestructionCallback(std::move(m_destructionCallback));

    // Initialize the session with the VM.
    RETURN_IF_FAILED(session->Initialize(Settings, Vm));

    // Create the service session ref. It extracts metadata and a weak reference from the session.
    auto serviceRef = Microsoft::WRL::Make<wslc::WSLCSessionReference>(session.Get());

    // Return the session as IWSLCSession interface
    RETURN_IF_FAILED(session->QueryInterface(IID_PPV_ARGS(Session)));
    *ServiceRef = serviceRef.Detach();

    WSL_LOG(
        "WSLCSessionFactoryCreatedSession",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(Settings->SessionId, "SessionId"),
        TraceLoggingWideString(Settings->DisplayName, "DisplayName"));

    return S_OK;
}
CATCH_RETURN()

HRESULT wslc::WSLCSessionFactory::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    wil::unique_handle process{OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, GetCurrentProcessId())};
    RETURN_LAST_ERROR_IF(!process);

    *ProcessHandle = process.release();
    return S_OK;
}
CATCH_RETURN()
