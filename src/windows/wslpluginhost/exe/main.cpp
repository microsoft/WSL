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

        // The PluginHost ctor/dtor pair manages the process keep-alive ref;
        // no manual AddComRef/ReleaseComRef needed here. If CopyTo fails, the
        // local ComPtr destructor releases the only reference, which destroys
        // the PluginHost and decrements the keep-alive count.
        RETURN_IF_FAILED(host.CopyTo(riid, ppCreated));
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

    // Harden the process before loading any third-party plugin code. Match the
    // mitigation set applied by the other WSL COM server processes
    // (wslservice.exe / wslcsession.exe).
    wsl::windows::common::security::ApplyProcessMitigationPolicies();

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

    // Bounded shutdown for orphaned hosts: if COM activates wslpluginhost.exe
    // but no client ever successfully creates an instance (e.g., the service
    // crashes between launch and CreateInstance, or activation is abandoned
    // by COM), exit instead of blocking on g_exitEvent forever. Once at least
    // one PluginHost has been constructed, AddComRef/ReleaseComRef govern
    // shutdown and we wait indefinitely for that ref count to drop to 0.
    constexpr DWORD c_startupTimeoutMs = 60'000;
    const DWORD waitResult = ::WaitForSingleObject(g_exitEvent.get(), c_startupTimeoutMs);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    if (waitResult == WAIT_TIMEOUT && wsl::windows::pluginhost::g_activationCount.load(std::memory_order_acquire) == 0)
    {
        LOG_HR_MSG(
            HRESULT_FROM_WIN32(ERROR_TIMEOUT),
            "wslpluginhost.exe startup timeout: no client activated the host within %u ms; exiting",
            c_startupTimeoutMs);
        return 1;
    }

    // Either the event was signaled, or a host was activated. Wait for the
    // exit signal (which fires when CoReleaseServerProcess returns 0).
    g_exitEvent.wait();

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 1;
}
