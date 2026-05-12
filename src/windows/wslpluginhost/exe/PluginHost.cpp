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
extern void AddComRef();
extern void ReleaseComRef();

std::atomic<wsl::windows::pluginhost::PluginHost*> wsl::windows::pluginhost::g_pluginHost{nullptr};

// Thread ID of the thread currently dispatching a plugin hook.
// Only that thread may call PluginError. Using thread ID instead of
// thread_local to avoid TLS initialization issues across DLL/EXE boundaries.
static std::atomic<DWORD> g_hookThreadId{0};

namespace {

// Plugins may invoke API stubs from worker threads they create themselves,
// which often haven't called CoInitializeEx. Without COM initialization, the
// cross-process m_callback proxy can't be used and calls would fail with
// CO_E_NOTINITIALIZED. This RAII helper joins the calling thread to the MTA
// for the duration of the API call. RPC_E_CHANGED_MODE means the thread is
// already STA-initialized; we leave its apartment unchanged. The supported
// path is an uninitialized worker thread joining the MTA where m_callback was
// marshaled — calling the proxy from a caller-created STA is not supported.
struct ScopedComInitForCallback
{
    HRESULT initHr;
    ScopedComInitForCallback() : initHr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    ~ScopedComInitForCallback()
    {
        if (SUCCEEDED(initHr))
        {
            ::CoUninitialize();
        }
    }
    ScopedComInitForCallback(const ScopedComInitForCallback&) = delete;
    ScopedComInitForCallback& operator=(const ScopedComInitForCallback&) = delete;

    HRESULT Result() const
    {
        return (initHr == RPC_E_CHANGED_MODE) ? S_OK : initHr;
    }
};

} // namespace

// Tracks how many PluginHost instances have ever been constructed in this
// process. Used by main() to distinguish "we were activated and the client
// disconnected normally" from "we sat here for the entire startup window
// without ever being activated by anyone" (orphaned host).
std::atomic<unsigned int> wsl::windows::pluginhost::g_activationCount{0};

PluginHost::PluginHost()
{
    // Increment the COM server reference count so the process stays alive while
    // this instance exists. Pairs with ReleaseComRef() in ~PluginHost(); tying
    // both to the object's lifetime guarantees they always balance regardless of
    // whether the factory's CopyTo() succeeds or fails.
    AddComRef();
    g_activationCount.fetch_add(1, std::memory_order_release);
}

PluginHost::~PluginHost()
{
    // Clear globally reachable state so late plugin API calls fail with
    // E_UNEXPECTED instead of dereferencing freed memory. Release-store
    // pairs with the acquire-loads in the Local* API stubs.
    PluginHost* expected = this;
    g_pluginHost.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

    // Module unloads automatically via wil::unique_hmodule destructor.

    // Decrement the COM server reference count. When it reaches zero,
    // the process will exit. Matches AddComRef() in PluginHost::PluginHost().
    ReleaseComRef();
}

// --- IWslPluginHost implementation ---

STDMETHODIMP PluginHost::Initialize(_In_ IWslPluginHostCallback* Callback, _In_ LPCWSTR PluginPath, _In_ LPCWSTR PluginName)
try
{
    RETURN_HR_IF(E_INVALIDARG, Callback == nullptr || PluginPath == nullptr || PluginName == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, m_module.is_valid()); // Already initialized

    m_callback = Callback;

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
        &LocalExecuteBinaryInDistribution,
        &LocalWslcMountFolder,
        &LocalWslcUnmountFolder,
        &LocalWslcCreateProcess,
        &LocalWslcProcessGetFd,
        &LocalWslcProcessGetExitEvent,
        &LocalWslcProcessGetExitCode,
        &LocalWslcReleaseProcess};

    // Publish g_pluginHost with release semantics so an acquire-load in a stub
    // observes a fully-constructed m_callback / m_hooks. Only publish on success
    // so a failed entry point never leaves a dangling pointer for late stub calls.
    g_pluginHost.store(this, std::memory_order_release);
    HRESULT hr = entryPoint(&api, &m_hooks);

    if (FAILED(hr))
    {
        g_pluginHost.store(nullptr, std::memory_order_release);
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

    std::lock_guard hookLock(m_hookLock);
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

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnVMStopping(&ctx.info);

    // Plugin must not call PluginError outside hooks that surface ErrorMessage;
    // silently drop any value so it can't poison a later fatal hook.
    m_pluginErrorMessage.reset();
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

    // Preserve nullability of PackageFamilyName/Flavor/Version per the public
    // ABI documented in WslPluginApi.h. Coalescing to L"" would silently change
    // behavior of plugins that check `if (Distribution->PackageFamilyName)`.
    WSLDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PidNamespace = PidNamespace;
    distro.PackageFamilyName = PackageFamilyName;
    distro.InitPid = InitPid;
    distro.Flavor = Flavor;
    distro.Version = Version;

    std::lock_guard hookLock(m_hookLock);
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
    distro.PackageFamilyName = PackageFamilyName;
    distro.InitPid = InitPid;
    distro.Flavor = Flavor;
    distro.Version = Version;

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionStopping(&ctx.info, &distro);

    m_pluginErrorMessage.reset();
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
    distro.PackageFamilyName = PackageFamilyName;
    distro.Flavor = Flavor;
    distro.Version = Version;

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionRegistered(&ctx.info, &distro);

    m_pluginErrorMessage.reset();
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
    distro.PackageFamilyName = PackageFamilyName;
    distro.Flavor = Flavor;
    distro.Version = Version;

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnDistributionUnregistered(&ctx.info, &distro);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

// --- Helpers ---

PluginHost::SessionContext PluginHost::BuildSessionContext(DWORD SessionId, HANDLE UserToken, DWORD SidSize, BYTE* SidData)
{
    SessionContext ctx{};
    ctx.info.SessionId = SessionId;

    // The marshaled UserToken is owned by the RPC stub, which closes it when the
    // call returns. Borrow it for the duration of the hook; do not take ownership.
    ctx.info.UserToken = UserToken;

    // Reconstruct the SID. Reject malformed inputs at the COM boundary —
    // pointer arithmetic on a null pointer (even with size 0) is undefined
    // behavior, and a malformed SID would be dereferenced by plugin code.
    THROW_HR_IF(E_INVALIDARG, SidData == nullptr || SidSize == 0);
    ctx.sidBuffer.assign(SidData, SidData + SidSize);
    THROW_HR_IF(E_INVALIDARG, !::IsValidSid(reinterpret_cast<PSID>(ctx.sidBuffer.data())));
    ctx.info.UserSid = reinterpret_cast<PSID>(ctx.sidBuffer.data());

    return ctx;
}

// --- Static API stubs ---

HRESULT CALLBACK PluginHost::LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto hr = host->m_callback->MountFolder(Session, WindowsPath, LinuxPath, ReadOnly, Name);
    return hr;
}

HRESULT CALLBACK PluginHost::LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    // Initialize the out-param so callers don't observe an uninitialized
    // socket value on any error return below.
    *Socket = INVALID_SOCKET;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

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
    HRESULT hr = host->m_callback->ExecuteBinary(Session, Path, count, Arguments, &socketResult);

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
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr)
    {
        // Not on a hook thread — PluginError must only be called
        // synchronously from within OnVMStarted/OnDistributionStarted.
        return E_ILLEGAL_METHOD_CALL;
    }

    RETURN_HR_IF(E_INVALIDARG, UserMessage == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, GetCurrentThreadId() != g_hookThreadId.load());
    RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, host->m_pluginErrorMessage.has_value());

    // Store locally — returned to service alongside the hook HRESULT.
    host->m_pluginErrorMessage.emplace(UserMessage);
    return S_OK;
}

HRESULT CALLBACK PluginHost::LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_INVALIDARG, Distro == nullptr);
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    // Initialize the out-param so callers don't observe an uninitialized
    // socket value on any error return below.
    *Socket = INVALID_SOCKET;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    DWORD count = 0;
    if (Arguments != nullptr)
    {
        for (const LPCSTR* p = Arguments; *p != nullptr; ++p)
        {
            ++count;
        }
    }

    HANDLE socketResult = nullptr;
    HRESULT hr = host->m_callback->ExecuteBinaryInDistribution(Session, Distro, Path, count, Arguments, &socketResult);

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

// --- WSLC hook implementations ---

PluginHost::WslcSessionContext PluginHost::BuildWslcSessionContext(
    DWORD SessionId, LPCWSTR DisplayName, DWORD ApplicationPid, HANDLE UserToken, DWORD SidSize, BYTE* SidData)
{
    WslcSessionContext ctx{};
    ctx.info.SessionId = SessionId;
    ctx.info.DisplayName = DisplayName;
    ctx.info.ApplicationPid = ApplicationPid;

    // UserToken / SidData are unique in the IDL — both may be null in stopping
    // paths invoked during session teardown. The marshaled token is owned by the
    // RPC stub (closed when the call returns), so borrow it without taking ownership.
    ctx.info.UserToken = UserToken;

    if (SidData != nullptr && SidSize > 0)
    {
        ctx.sidBuffer.assign(SidData, SidData + SidSize);
        THROW_HR_IF(E_INVALIDARG, !::IsValidSid(reinterpret_cast<PSID>(ctx.sidBuffer.data())));
        ctx.info.UserSid = reinterpret_cast<PSID>(ctx.sidBuffer.data());
    }

    return ctx;
}

STDMETHODIMP PluginHost::OnWslcSessionCreated(
    _In_ DWORD SessionId,
    _In_ LPCWSTR DisplayName,
    _In_ DWORD ApplicationPid,
    _In_opt_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_opt_(SidSize) BYTE* SidData,
    _Outptr_result_maybenull_ LPWSTR* ErrorMessage)
try
{
    RETURN_HR_IF(E_POINTER, ErrorMessage == nullptr);
    *ErrorMessage = nullptr;

    if (m_hooks.OnSessionCreated == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnSessionCreated(&ctx.info);

    if (m_pluginErrorMessage.has_value())
    {
        *ErrorMessage = wil::make_cotaskmem_string(m_pluginErrorMessage->c_str()).release();
        m_pluginErrorMessage.reset();
    }

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcSessionStopping(
    _In_ DWORD SessionId, _In_ LPCWSTR DisplayName, _In_ DWORD ApplicationPid, _In_opt_ HANDLE UserToken, _In_ DWORD SidSize, _In_reads_opt_(SidSize) BYTE* SidData)
try
{
    if (m_hooks.OnSessionStopping == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.OnSessionStopping(&ctx.info);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcContainerStarted(
    _In_ DWORD SessionId,
    _In_ LPCWSTR DisplayName,
    _In_ DWORD ApplicationPid,
    _In_opt_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_opt_(SidSize) BYTE* SidData,
    _In_ LPCSTR InspectJson,
    _Outptr_result_maybenull_ LPWSTR* ErrorMessage)
try
{
    RETURN_HR_IF(E_POINTER, ErrorMessage == nullptr);
    *ErrorMessage = nullptr;

    if (m_hooks.ContainerStarted == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.ContainerStarted(&ctx.info, InspectJson);

    if (m_pluginErrorMessage.has_value())
    {
        *ErrorMessage = wil::make_cotaskmem_string(m_pluginErrorMessage->c_str()).release();
        m_pluginErrorMessage.reset();
    }

    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcContainerStopping(
    _In_ DWORD SessionId,
    _In_ LPCWSTR DisplayName,
    _In_ DWORD ApplicationPid,
    _In_opt_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_opt_(SidSize) BYTE* SidData,
    _In_ LPCSTR ContainerId)
try
{
    if (m_hooks.ContainerStopping == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.ContainerStopping(&ctx.info, ContainerId);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcImageCreated(
    _In_ DWORD SessionId,
    _In_ LPCWSTR DisplayName,
    _In_ DWORD ApplicationPid,
    _In_opt_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_opt_(SidSize) BYTE* SidData,
    _In_ LPCSTR InspectJson)
try
{
    if (m_hooks.ImageCreated == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.ImageCreated(&ctx.info, InspectJson);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcImageDeleted(
    _In_ DWORD SessionId,
    _In_ LPCWSTR DisplayName,
    _In_ DWORD ApplicationPid,
    _In_opt_ HANDLE UserToken,
    _In_ DWORD SidSize,
    _In_reads_opt_(SidSize) BYTE* SidData,
    _In_ LPCSTR ImageId)
try
{
    if (m_hooks.ImageDeleted == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    std::lock_guard hookLock(m_hookLock);
    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.ImageDeleted(&ctx.info, ImageId);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

// --- WSLC API stubs ---

namespace {

// Opaque wrapper handed to the plugin as WSLCProcessHandle. The DWORD cookie
// identifies the IWSLCProcess held by the service-side PluginHostCallbackImpl.
struct WslcProcessWrapper
{
    DWORD cookie;
};

} // namespace

HRESULT CALLBACK PluginHost::LocalWslcMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, LPCSTR Mountpoint, BOOL ReadOnly)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    return host->m_callback->WslcMountFolder(Session, WindowsPath, Mountpoint, ReadOnly);
}

HRESULT CALLBACK PluginHost::LocalWslcUnmountFolder(WSLCSessionId Session, LPCSTR Mountpoint)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    return host->m_callback->WslcUnmountFolder(Session, Mountpoint);
}

HRESULT CALLBACK PluginHost::LocalWslcCreateProcess(
    WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcessHandle* Process, int* Errno)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_POINTER, Process == nullptr);
    *Process = nullptr;

    int localErrno = 0;
    if (Errno != nullptr)
    {
        *Errno = 0;
    }

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    DWORD argCount = 0;
    if (Arguments != nullptr)
    {
        for (const LPCSTR* p = Arguments; *p != nullptr; ++p)
        {
            ++argCount;
        }
    }

    DWORD envCount = 0;
    if (Env != nullptr)
    {
        for (const LPCSTR* p = Env; *p != nullptr; ++p)
        {
            ++envCount;
        }
    }

    // Allocate the wrapper before creating the remote process so a throwing
    // allocation can't strand a service-side cookie that only WslcReleaseProcess
    // frees. Nothing between the remote create and release() below can throw.
    auto wrapper = std::make_unique<WslcProcessWrapper>();

    DWORD cookie = 0;
    HRESULT hr = host->m_callback->WslcCreateProcess(Session, Executable, argCount, Arguments, envCount, Env, &cookie, &localErrno);
    if (Errno != nullptr)
    {
        *Errno = localErrno;
    }

    if (FAILED(hr))
    {
        return hr;
    }

    wrapper->cookie = cookie;
    *Process = wrapper.release();
    return S_OK;
}

HRESULT CALLBACK PluginHost::LocalWslcProcessGetFd(WSLCProcessHandle Process, WSLCProcessFd Fd, HANDLE* Handle)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, Handle == nullptr);
    *Handle = nullptr;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);
    return host->m_callback->WslcProcessGetFd(wrapper->cookie, static_cast<DWORD>(Fd), Handle);
}

HRESULT CALLBACK PluginHost::LocalWslcProcessGetExitEvent(WSLCProcessHandle Process, HANDLE* ExitEvent)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, ExitEvent == nullptr);
    *ExitEvent = nullptr;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);
    return host->m_callback->WslcProcessGetExitEvent(wrapper->cookie, ExitEvent);
}

HRESULT CALLBACK PluginHost::LocalWslcProcessGetExitCode(WSLCProcessHandle Process, int* ExitCode)
{
    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return E_UNEXPECTED;
    }

    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, ExitCode == nullptr);

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);
    return host->m_callback->WslcProcessGetExitCode(wrapper->cookie, ExitCode);
}

void CALLBACK PluginHost::LocalWslcReleaseProcess(WSLCProcessHandle Process)
{
    if (Process == nullptr)
    {
        return;
    }

    std::unique_ptr<WslcProcessWrapper> wrapper{static_cast<WslcProcessWrapper*>(Process)};

    auto* host = g_pluginHost.load(std::memory_order_acquire);
    if (host == nullptr || host->m_callback == nullptr)
    {
        return;
    }

    ScopedComInitForCallback coInit;
    if (FAILED(coInit.Result()))
    {
        return;
    }

    LOG_IF_FAILED(host->m_callback->WslcReleaseProcess(wrapper->cookie));
}