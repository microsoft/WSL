/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    Entry point for wslasession.exe - the per-user COM server for WSLA sessions.

    This runs under the user's identity and hosts WSLASessionFactory COM objects.
    The SYSTEM service creates sessions via IWSLASessionFactory::CreateSession,
    which returns both the session and a service reference for lifetime tracking.

--*/

#include "precomp.h"
#include "WSLASessionFactory.h"

using namespace wsl::windows::common;
using namespace wsl::windows::common::wslutil;

namespace {

// Event used to signal that the COM server should exit.
wil::unique_event g_exitEvent{wil::EventOptions::ManualReset};

class WSLASessionFactoryClassFactory : public winrt::implements<WSLASessionFactoryClassFactory, IClassFactory>
{
public:
    STDMETHODIMP CreateInstance(_In_ IUnknown* outer, REFIID iid, _COM_Outptr_ void** result) noexcept override
    try
    {
        *result = nullptr;
        THROW_HR_IF(CLASS_E_NOAGGREGATION, outer != nullptr);

        auto factory = Microsoft::WRL::Make<wsl::windows::service::wsla::WSLASessionFactory>();
        factory->SetDestructionCallback([]() { g_exitEvent.SetEvent(); });
        return factory->QueryInterface(iid, result);
    }
    CATCH_RETURN()

    STDMETHODIMP LockServer(BOOL) noexcept override
    {
        return S_OK;
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
try
{
    ConfigureCrt();

    // Enable contextualized errors
    wsl::windows::common::EnableContextualizedErrors(true);

    // Initialize telemetry
    WslTraceLoggingInitialize(WslaTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanup = wil::scope_exit([] { WslTraceLoggingUninitialize(); });

    wsl::windows::common::wslutil::ConfigureCrashHandler();

    // Don't kill the process on unknown C++ exceptions
    wil::g_fResultFailFastUnknownExceptions = false;

    wsl::windows::common::security::ApplyProcessMitigationPolicies();

    // Initialize Winsock
    WSADATA data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));

    WSL_LOG("Per-user session server starting", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Initialize COM
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wsl::windows::common::wslutil::CoInitializeSecurity();

    // Register the class factory (single-use: one factory per process)
    auto factory = winrt::make<WSLASessionFactoryClassFactory>();
    wil::unique_com_class_object_cookie cookie;
    THROW_IF_FAILED(::CoRegisterClassObject(
        __uuidof(wsl::windows::service::wsla::WSLASessionFactory), factory.get(), CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &cookie));

    WSL_LOG("Per-user session server registered, waiting for activations", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Wait until all objects have been released
    g_exitEvent.wait();

    WSL_LOG("Per-user session server exiting", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 1;
}
