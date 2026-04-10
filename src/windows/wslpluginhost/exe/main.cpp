/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entry point for wslpluginhost.exe.
    This process acts as a COM local server that loads a single WSL plugin DLL
    in an isolated process, preventing a buggy or malicious plugin from crashing
    the main WSL service.

    The host is activated through COM local-server activation. It registers its
    COM class factory, serves activation requests, and remains alive until all
    COM server-process references are released, at which point it exits.

--*/

#include "precomp.h"
#include "PluginHost.h"
#include "WslPluginHost.h"

using namespace Microsoft::WRL;

static wil::unique_event g_exitEvent(wil::EventOptions::ManualReset);

void AddComRef()
{
    CoAddRefServerProcess();
}

void ReleaseComRef()
{
    if (CoReleaseServerProcess() == 0)
    {
        g_exitEvent.SetEvent();
    }
}

class PluginHostFactory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory>
{
public:
    STDMETHODIMP CreateInstance(_In_opt_ IUnknown* pUnkOuter, _In_ REFIID riid, _Outptr_ void** ppCreated) override
    try
    {
        RETURN_HR_IF_NULL(E_POINTER, ppCreated);
        *ppCreated = nullptr;
        RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

        auto host = Make<wsl::windows::pluginhost::PluginHost>();
        RETURN_IF_NULL_ALLOC(host);

        AddComRef();
        auto releaseOnFailure = wil::scope_exit([] { ReleaseComRef(); });
        RETURN_IF_FAILED(host.CopyTo(riid, ppCreated));
        releaseOnFailure.release();
        return S_OK;
    }
    CATCH_RETURN();

    STDMETHODIMP LockServer(BOOL lock) noexcept override
    {
        if (lock)
        {
            AddComRef();
        }
        else
        {
            ReleaseComRef();
        }
        return S_OK;
    }
};

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
try
{
    wsl::windows::common::wslutil::ConfigureCrt();
    wsl::windows::common::wslutil::InitializeWil();

    // Initialize logging.
    WslTraceLoggingInitialize(WslServiceTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanupTracing = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { WslTraceLoggingUninitialize(); });

    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wsl::windows::common::wslutil::CoInitializeSecurity();

    // Initialize Winsock — plugins receive sockets from ExecuteBinary and need
    // Winsock to be initialized for recv/send/closesocket to work.
    WSADATA wsaData{};
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &wsaData));
    auto cleanupWinsock = wil::scope_exit([] { WSACleanup(); });

    // Register the class factory so the service can CoCreateInstance on us.
    DWORD cookie = 0;
    auto factory = Make<PluginHostFactory>();
    THROW_IF_NULL_ALLOC(factory);

    THROW_IF_FAILED(::CoRegisterClassObject(CLSID_WslPluginHost, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_SINGLEUSE, &cookie));

    auto revokeOnExit = wil::scope_exit([&]() { ::CoRevokeClassObject(cookie); });

    // Wait until the COM reference count drops to zero.
    g_exitEvent.wait();

    return 0;
}
CATCH_RETURN();
