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
    _In_ const WSLCSessionInitSettings* Settings,
    _In_ IWSLCVirtualMachineFactory* VmFactory,
    _In_ IWSLCPluginNotifier* PluginNotifier,
    _In_opt_ IWarningCallback* WarningCallback,
    _Out_ IWSLCSession** Session,
    _Out_ IWSLCSessionReference** ServiceRef)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Session);
    RETURN_HR_IF_NULL(E_POINTER, ServiceRef);

    *Session = nullptr;
    *ServiceRef = nullptr;

    // Create the session object.
    auto session = Microsoft::WRL::Make<wslc::WSLCSession>();

    // Initialize the session with the VM factory (VMs are created on demand).
    RETURN_IF_FAILED(session->Initialize(Settings, VmFactory, PluginNotifier, WarningCallback));

    // Create the service session ref. It extracts metadata and a weak reference from the session.
    auto serviceRef = Microsoft::WRL::Make<wslc::WSLCSessionReference>(session.Get());

    // Return the session as IWSLCSession interface
    RETURN_IF_FAILED(session->QueryInterface(IID_PPV_ARGS(Session)));
    *ServiceRef = serviceRef.Detach();

    // N.B. The destruction callback must be installed last, after all fallible operations.
    // If installed earlier, an unwinding session local would fire the exit callback and race
    // with the COM stub marshaling IErrorInfo back to the caller.
    session->SetDestructionCallback(std::move(m_destructionCallback));

    WSL_LOG(
        "WSLCSessionFactoryCreatedSession",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(Settings->SessionId, "SessionId"),
        TraceLoggingWideString(Settings->DisplayName, "DisplayName"));

    return S_OK;
}
CATCH_RETURN()

HRESULT wslc::WSLCSessionFactory::InterfaceSupportsErrorInfo(_In_ REFIID riid)
{
    return riid == __uuidof(IWSLCSessionFactory) ? S_OK : S_FALSE;
}

HRESULT wslc::WSLCSessionFactory::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    RETURN_HR_IF_NULL(E_POINTER, ProcessHandle);

    *ProcessHandle = wslutil::DuplicateHandle(GetCurrentProcess(), PROCESS_SET_QUOTA | PROCESS_TERMINATE);
    return S_OK;
}
CATCH_RETURN()
