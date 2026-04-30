/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginHost.cpp

Abstract:

    This file contains the IWslPluginHost COM class implementation.
    It loads a plugin DLL in this (host) process and forwards lifecycle
    notifications from the service to the plugin, while routing plugin API
    callbacks back to the service via IWslPluginHostCallback.

--*/

#include "precomp.h"
#include "PluginHost.h"
#include "install.h"

using namespace wsl::windows::pluginhost;

// Defined in main.cpp — part of the COM local server lifecycle.
extern void ReleaseComRef();

PluginHost* wsl::windows::pluginhost::g_pluginHost = nullptr;

// Thread ID of the thread currently dispatching a plugin hook.
// Only that thread may call PluginError. Using thread ID instead of
// thread_local to avoid TLS initialization issues across DLL/EXE boundaries.
static std::atomic<DWORD> g_hookThreadId{0};

PluginHost::~PluginHost()
{
    // Clear globally reachable state so late plugin API calls fail with
    // E_UNEXPECTED instead of dereferencing freed memory.
    if (g_pluginHost == this)
    {
        g_pluginHost = nullptr;
    }

    // Module unloads automatically via wil::unique_hmodule destructor.

    // Decrement the COM server reference count. When it reaches zero,
    // the process will exit. Matches AddComRef() in PluginHostFactory::CreateInstance.
    ReleaseComRef();
}

// --- IWslPluginHost implementation ---

STDMETHODIMP PluginHost::Initialize(_In_ IWslPluginHostCallback* Callback, _In_ LPCWSTR PluginPath, _In_ LPCWSTR PluginName)
try
{
    RETURN_HR_IF(E_INVALIDARG, Callback == nullptr || PluginPath == nullptr || PluginName == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, m_module.is_valid()); // Already initialized

    m_callback = Callback;
    m_pluginName = PluginName;

    // Validate the plugin signature before loading it.
    // Keep the file handle open to prevent TOCTOU (swap between validation and load).
    wil::unique_hfile signatureHandle;
    if constexpr (wsl::shared::OfficialBuild)
    {
        signatureHandle = wsl::windows::common::install::ValidateFileSignature(PluginPath);
    }

    m_module.reset(LoadLibrary(PluginPath));
    THROW_LAST_ERROR_IF_NULL(m_module);
    signatureHandle.reset(); // Safe to release after LoadLibrary has mapped the DLL

    const auto entryPoint =
        reinterpret_cast<WSLPluginAPI_EntryPointV1>(GetProcAddress(m_module.get(), GSL_STRINGIFY(WSLPLUGINAPI_ENTRYPOINTV1)));
    THROW_LAST_ERROR_IF_NULL(entryPoint);

    // Build the API vtable that the plugin will use to call back into the service.
    // The function pointers are static methods on this class that route through g_pluginHost.
    static const WSLPluginAPIV1 api = {
        {wsl::shared::VersionMajor, wsl::shared::VersionMinor, wsl::shared::VersionRevision},
        &LocalMountFolder,
        &LocalExecuteBinary,
        &LocalPluginError,
        &LocalExecuteBinaryInDistribution};

    g_pluginHost = this;
    HRESULT hr = entryPoint(&api, &m_hooks);

    if (FAILED(hr))
    {
        g_pluginHost = nullptr;
        RETURN_HR_MSG(hr, "Plugin entry point failed: '%ls'", PluginPath);
    }

    return S_OK;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
try
{
    RETURN_HR_IF(E_POINTER, ProcessHandle == nullptr);
    *ProcessHandle = nullptr;

    wil::unique_handle process(OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, GetCurrentProcessId()));
    RETURN_LAST_ERROR_IF_NULL(process);

    // COM's system_handle(sh_process) marshaling will duplicate this into the caller's process.
    *ProcessHandle = process.release();
    return S_OK;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnVMStarted(
    _In_ DWORD SessionId,
    _In_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_(SidSize) BYTE* SidData,
    _In_ DWORD CustomConfigurationFlags,
    _Outptr_result_maybenull_ LPWSTR* ErrorMessage)
try
{
    RETURN_HR_IF(E_POINTER, ErrorMessage == nullptr);
    *ErrorMessage = nullptr;

    if (m_hooks.OnVMStarted == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);
    WSLVmCreationSettings settings{};
    settings.CustomConfigurationFlags = static_cast<WSLUserConfiguration>(CustomConfigurationFlags);

    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnVMStarted(&ctx.info, &settings);

    // If the plugin called PluginError during the hook, return the message.
    if (m_pluginErrorMessage.has_value())
    {
        *ErrorMessage = wil::make_cotaskmem_string(m_pluginErrorMessage->c_str()).release();
        m_pluginErrorMessage.reset();
    }

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnVMStopping(_In_ DWORD SessionId, _In_ HANDLE UserToken, _In_ DWORD SidSize, _In_reads_(SidSize) BYTE* SidData)
try
{
    if (m_hooks.OnVMStopping == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);

    g_hookThreadId.store(GetCurrentThreadId());
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnVMStopping(&ctx.info);

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnDistributionStarted(
    _In_ DWORD SessionId,
    _In_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_(SidSize) BYTE* SidData,
    _In_ const GUID* DistributionId,
    _In_ LPCWSTR DistributionName,
    _In_ ULONGLONG PidNamespace,
    _In_opt_ LPCWSTR PackageFamilyName,
    _In_ DWORD InitPid,
    _In_opt_ LPCWSTR Flavor,
    _In_opt_ LPCWSTR Version,
    _Outptr_result_maybenull_ LPWSTR* ErrorMessage)
try
{
    RETURN_HR_IF(E_POINTER, ErrorMessage == nullptr);
    *ErrorMessage = nullptr;

    if (m_hooks.OnDistributionStarted == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);

    WSLDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PidNamespace = PidNamespace;
    distro.PackageFamilyName = PackageFamilyName ? PackageFamilyName : L"";
    distro.InitPid = InitPid;
    distro.Flavor = Flavor ? Flavor : L"";
    distro.Version = Version ? Version : L"";

    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionStarted(&ctx.info, &distro);

    if (m_pluginErrorMessage.has_value())
    {
        *ErrorMessage = wil::make_cotaskmem_string(m_pluginErrorMessage->c_str()).release();
        m_pluginErrorMessage.reset();
    }

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnDistributionStopping(
    _In_ DWORD SessionId,
    _In_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_(SidSize) BYTE* SidData,
    _In_ const GUID* DistributionId,
    _In_ LPCWSTR DistributionName,
    _In_ ULONGLONG PidNamespace,
    _In_opt_ LPCWSTR PackageFamilyName,
    _In_ DWORD InitPid,
    _In_opt_ LPCWSTR Flavor,
    _In_opt_ LPCWSTR Version)
try
{
    if (m_hooks.OnDistributionStopping == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);

    WSLDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PidNamespace = PidNamespace;
    distro.PackageFamilyName = PackageFamilyName ? PackageFamilyName : L"";
    distro.InitPid = InitPid;
    distro.Flavor = Flavor ? Flavor : L"";
    distro.Version = Version ? Version : L"";

    g_hookThreadId.store(GetCurrentThreadId());
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionStopping(&ctx.info, &distro);

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnDistributionRegistered(
    _In_ DWORD SessionId,
    _In_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_(SidSize) BYTE* SidData,
    _In_ const GUID* DistributionId,
    _In_ LPCWSTR DistributionName,
    _In_opt_ LPCWSTR PackageFamilyName,
    _In_opt_ LPCWSTR Flavor,
    _In_opt_ LPCWSTR Version)
try
{
    if (m_hooks.OnDistributionRegistered == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);

    WslOfflineDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PackageFamilyName = PackageFamilyName ? PackageFamilyName : L"";
    distro.Flavor = Flavor ? Flavor : L"";
    distro.Version = Version ? Version : L"";

    g_hookThreadId.store(GetCurrentThreadId());
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionRegistered(&ctx.info, &distro);

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnDistributionUnregistered(
    _In_ DWORD SessionId,
    _In_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_(SidSize) BYTE* SidData,
    _In_ const GUID* DistributionId,
    _In_ LPCWSTR DistributionName,
    _In_opt_ LPCWSTR PackageFamilyName,
    _In_opt_ LPCWSTR Flavor,
    _In_opt_ LPCWSTR Version)
try
{
    if (m_hooks.OnDistributionUnregistered == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildSessionContext(SessionId, UserToken, SidSize, SidData);

    WslOfflineDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PackageFamilyName = PackageFamilyName ? PackageFamilyName : L"";
    distro.Flavor = Flavor ? Flavor : L"";
    distro.Version = Version ? Version : L"";

    g_hookThreadId.store(GetCurrentThreadId());
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionUnregistered(&ctx.info, &distro);

    return hr;
}
CATCH_RETURN();

// --- Helpers ---

PluginHost::SessionContext PluginHost::BuildSessionContext(DWORD SessionId, HANDLE UserToken, DWORD SidSize, BYTE* SidData)
{
    SessionContext ctx{};
    ctx.info.SessionId = SessionId;

    // COM's system_handle marshaling automatically duplicated the token into our process.
    // Take ownership of the duplicated handle.
    ctx.tokenHandle.reset(UserToken);
    ctx.info.UserToken = ctx.tokenHandle.get();

    // Reconstruct the SID from the serialized bytes.
    ctx.sidBuffer.assign(SidData, SidData + SidSize);
    ctx.info.UserSid = reinterpret_cast<PSID>(ctx.sidBuffer.data());

    return ctx;
}

// --- Static API stubs ---

HRESULT CALLBACK PluginHost::LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name)
{
    if (g_pluginHost == nullptr || g_pluginHost->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    auto hr = g_pluginHost->m_callback->MountFolder(Session, WindowsPath, LinuxPath, ReadOnly, Name);
    return hr;
}

HRESULT CALLBACK PluginHost::LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
{
    if (g_pluginHost == nullptr || g_pluginHost->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_POINTER, Socket == nullptr);

    // Count arguments (NULL-terminated array)
    DWORD count = 0;
    if (Arguments != nullptr)
    {
        for (const LPCSTR* p = Arguments; *p != nullptr; ++p)
        {
            ++count;
        }
    }

    HANDLE socketResult = nullptr;
    HRESULT hr = g_pluginHost->m_callback->ExecuteBinary(Session, Path, count, Arguments, &socketResult);

    if (SUCCEEDED(hr))
    {
        // COM's system_handle marshaling duplicated the socket into our process.
        *Socket = reinterpret_cast<SOCKET>(socketResult);
    }
    else if (socketResult != nullptr)
    {
        if (closesocket(reinterpret_cast<SOCKET>(socketResult)) == SOCKET_ERROR)
        {
            LOG_WIN32(WSAGetLastError());
        }
    }

    return hr;
}

HRESULT CALLBACK PluginHost::LocalPluginError(LPCWSTR UserMessage)
{
    if (g_pluginHost == nullptr)
    {
        // Not on a hook thread — PluginError must only be called
        // synchronously from within OnVMStarted/OnDistributionStarted.
        return E_ILLEGAL_METHOD_CALL;
    }

    RETURN_HR_IF(E_INVALIDARG, UserMessage == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, GetCurrentThreadId() != g_hookThreadId.load());
    RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, g_pluginHost->m_pluginErrorMessage.has_value());

    // Store locally — returned to service alongside the hook HRESULT.
    g_pluginHost->m_pluginErrorMessage.emplace(UserMessage);
    return S_OK;
}

HRESULT CALLBACK PluginHost::LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
{
    if (g_pluginHost == nullptr || g_pluginHost->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_INVALIDARG, Distro == nullptr);
    RETURN_HR_IF(E_POINTER, Socket == nullptr);

    DWORD count = 0;
    if (Arguments != nullptr)
    {
        for (const LPCSTR* p = Arguments; *p != nullptr; ++p)
        {
            ++count;
        }
    }

    HANDLE socketResult = nullptr;
    HRESULT hr = g_pluginHost->m_callback->ExecuteBinaryInDistribution(Session, Distro, Path, count, Arguments, &socketResult);

    if (SUCCEEDED(hr))
    {
        *Socket = reinterpret_cast<SOCKET>(socketResult);
    }
    else if (socketResult != nullptr)
    {
        if (closesocket(reinterpret_cast<SOCKET>(socketResult)) == SOCKET_ERROR)
        {
            LOG_WIN32(WSAGetLastError());
        }
    }

    return hr;
}
