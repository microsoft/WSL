// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginHost.h"
#include "install.h"

using namespace wsl::windows::pluginhost;

// Defined in main.cpp — part of the COM local server lifecycle.
extern void AddComRef();
extern void ReleaseComRef();

std::atomic<wsl::windows::pluginhost::PluginHost*> wsl::windows::pluginhost::g_pluginHost{nullptr};

// Keep error reporting local to each concurrently executing notification.
static thread_local std::atomic<DWORD> g_hookThreadId{0};
thread_local std::optional<std::wstring> PluginHost::m_pluginErrorMessage;
static wil::srwlock g_hostLock;

namespace {

// Plugin worker threads need COM initialization; existing apartments are preserved.
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
    NON_COPYABLE(ScopedComInitForCallback);
    NON_MOVABLE(ScopedComInitForCallback);

    HRESULT Result() const
    {
        return (initHr == RPC_E_CHANGED_MODE) ? S_OK : initHr;
    }
};

} // namespace

// Distinguishes an abandoned activation from a live server at the startup timeout.
std::atomic<unsigned int> wsl::windows::pluginhost::g_activationCount{0};

PluginHost::PluginHost()
{
    AddComRef();
    g_activationCount.fetch_add(1, std::memory_order_release);
}

PluginHost::~PluginHost()
{
    {
        auto lock = g_hostLock.lock_exclusive();
        PluginHost* expected = this;
        g_pluginHost.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
    if (m_callbackCookie != 0)
    {
        LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(m_callbackCookie));
    }
    // Plugin-owned threads may still be executing its code. Process exit unloads it.
    static_cast<void>(m_module.release());

    ReleaseComRef();
}

// --- IWslPluginHost implementation ---

STDMETHODIMP PluginHost::JoinJob(_In_ HANDLE JobObject)
try
{
    RETURN_HR_IF(E_INVALIDARG, JobObject == nullptr);
    RETURN_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(JobObject, GetCurrentProcess()));
    m_inJob = true;
    return S_OK;
}
CATCH_RETURN()

STDMETHODIMP PluginHost::Initialize(_In_ IWslPluginHostCallback* Callback, _In_ LPCWSTR PluginPath, _In_ LPCWSTR PluginName)
try
{
    RETURN_HR_IF(E_INVALIDARG, Callback == nullptr || PluginPath == nullptr || PluginName == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, !m_inJob);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, m_module.is_valid()); // Already initialized

    THROW_IF_FAILED(CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_git)));
    THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(Callback, __uuidof(IWslPluginHostCallback), &m_callbackCookie));

    // Validate the plugin signature before loading it.
    // Keep the file handle open to prevent TOCTOU (swap between validation and load).
    wil::unique_hfile signatureHandle;
    if constexpr (wsl::shared::OfficialBuild)
    {
        signatureHandle = wsl::windows::common::install::ValidateFileSignature(PluginPath);
    }

    // Exclude the working directory from dependency resolution in this SYSTEM process.
    m_module.reset(LoadLibraryExW(PluginPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
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

    // Entry points may call the API, so publish the initialized callback first.
    {
        auto lock = g_hostLock.lock_exclusive();
        g_pluginHost.store(this, std::memory_order_release);
    }
    HRESULT hr = entryPoint(&api, &m_hooks);

    RETURN_IF_FAILED_MSG(hr, "Plugin entry point failed: '%ls'", PluginPath);

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

    // Preserve optional strings as null, as required by the public plugin ABI.
    WSLDistributionInformation distro{};
    distro.Id = *DistributionId;
    distro.Name = DistributionName;
    distro.PidNamespace = PidNamespace;
    distro.PackageFamilyName = PackageFamilyName;
    distro.InitPid = InitPid;
    distro.Flavor = Flavor;
    distro.Version = Version;

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

    // Validate the SID header and size before copying or dereferencing it.
    THROW_HR_IF(E_INVALIDARG, SidData == nullptr || SidSize == 0);
    THROW_HR_IF(E_INVALIDARG, SidSize < offsetof(SID, SubAuthority));
    THROW_HR_IF(E_INVALIDARG, SidSize < GetSidLengthRequired(SidData[1]));
    ctx.sidBuffer.assign(SidData, SidData + SidSize);
    THROW_HR_IF(E_INVALIDARG, !::IsValidSid(reinterpret_cast<PSID>(ctx.sidBuffer.data())));
    ctx.info.UserSid = reinterpret_cast<PSID>(ctx.sidBuffer.data());

    return ctx;
}

// --- Static API stubs ---

wil::com_ptr<IWslPluginHostCallback> PluginHost::GetCallback()
{
    auto lock = g_hostLock.lock_shared();
    const auto* host = g_pluginHost.load(std::memory_order_acquire);
    THROW_HR_IF(RPC_E_DISCONNECTED, host == nullptr);
    wil::com_ptr<IWslPluginHostCallback> callback;
    THROW_IF_FAILED(host->m_git->GetInterfaceFromGlobal(host->m_callbackCookie, IID_PPV_ARGS(&callback)));
    return callback;
}

HRESULT CALLBACK PluginHost::LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name)
try
{
    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    return GetCallback()->MountFolder(Session, WindowsPath, LinuxPath, ReadOnly, Name);
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
try
{
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
    HRESULT hr = GetCallback()->ExecuteBinary(Session, Path, count, Arguments, &socketResult);

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
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalPluginError(LPCWSTR UserMessage)
try
{
    RETURN_HR_IF(E_INVALIDARG, UserMessage == nullptr);
    RETURN_HR_IF(E_ILLEGAL_METHOD_CALL, GetCurrentThreadId() != g_hookThreadId.load());
    RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, m_pluginErrorMessage.has_value());

    // Store locally — returned to service alongside the hook HRESULT.
    m_pluginErrorMessage.emplace(UserMessage);
    return S_OK;
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
try
{
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
    HRESULT hr = GetCallback()->ExecuteBinaryInDistribution(Session, Distro, Path, count, Arguments, &socketResult);

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
CATCH_RETURN()

// --- WSLC hook implementations ---

PluginHost::WslcSessionContext PluginHost::BuildWslcSessionContext(
    DWORD SessionId, LPCWSTR DisplayName, DWORD ApplicationPid, HANDLE UserToken, DWORD SidSize, BYTE* SidData)
{
    WslcSessionContext ctx{};
    ctx.info.SessionId = SessionId;
    ctx.info.DisplayName = DisplayName;
    ctx.info.ApplicationPid = ApplicationPid;

    // The marshaler owns the optional token for the duration of this notification.
    ctx.info.UserToken = UserToken;

    if (SidData != nullptr && SidSize > 0)
    {
        THROW_HR_IF(E_INVALIDARG, SidSize < offsetof(SID, SubAuthority));
        THROW_HR_IF(E_INVALIDARG, SidSize < GetSidLengthRequired(SidData[1]));
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

    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.ImageDeleted(&ctx.info, ImageId);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcVmStarted(
    _In_ DWORD SessionId, _In_ LPCWSTR DisplayName, _In_ DWORD ApplicationPid, _In_opt_ HANDLE UserToken, _In_ DWORD SidSize, _In_reads_opt_(SidSize) BYTE* SidData)
try
{
    if (m_hooks.WslcVmStarted == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.WslcVmStarted(&ctx.info);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

STDMETHODIMP PluginHost::OnWslcVmStopping(
    _In_ DWORD SessionId, _In_ LPCWSTR DisplayName, _In_ DWORD ApplicationPid, _In_opt_ HANDLE UserToken, _In_ DWORD SidSize, _In_reads_opt_(SidSize) BYTE* SidData)
try
{
    if (m_hooks.WslcVmStopping == nullptr)
    {
        return S_OK;
    }

    auto ctx = BuildWslcSessionContext(SessionId, DisplayName, ApplicationPid, UserToken, SidSize, SidData);

    g_hookThreadId.store(GetCurrentThreadId());
    m_pluginErrorMessage.reset();
    auto cleanup = wil::scope_exit([&] { g_hookThreadId.store(0); });

    HRESULT hr = m_hooks.WslcVmStopping(&ctx.info);

    m_pluginErrorMessage.reset();
    return hr;
}
CATCH_RETURN();

// --- WSLC API stubs ---

namespace {

// Opaque wrapper handed to the plugin as WSLCProcessHandle. It owns the
// IWSLCProcess COM proxy marshaled from wslcsession (via the service), so the
// host calls the process interface directly and the remote process is released
// when the plugin releases this wrapper (or when the host process exits).
struct WslcProcessWrapper
{
    wil::com_ptr<IGlobalInterfaceTable> git;
    DWORD cookie = 0;

    ~WslcProcessWrapper()
    {
        if (cookie != 0)
        {
            LOG_IF_FAILED(git->RevokeInterfaceFromGlobal(cookie));
        }
    }

    wil::com_ptr<IWSLCProcess> GetProcess() const
    {
        wil::com_ptr<IWSLCProcess> process;
        THROW_IF_FAILED(git->GetInterfaceFromGlobal(cookie, IID_PPV_ARGS(&process)));
        return process;
    }
};

} // namespace

HRESULT CALLBACK PluginHost::LocalWslcMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, LPCSTR Mountpoint, BOOL ReadOnly)
try
{
    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    return GetCallback()->WslcMountFolder(Session, WindowsPath, Mountpoint, ReadOnly);
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalWslcUnmountFolder(WSLCSessionId Session, LPCSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    return GetCallback()->WslcUnmountFolder(Session, Mountpoint);
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalWslcCreateProcess(
    WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcessHandle* Process, int* Errno)
try
{
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

    auto wrapper = std::make_unique<WslcProcessWrapper>();
    THROW_IF_FAILED(CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wrapper->git)));

    wil::com_ptr<IWSLCProcess> process;
    HRESULT hr = GetCallback()->WslcCreateProcess(Session, Executable, argCount, Arguments, envCount, Env, &process, &localErrno);
    if (Errno != nullptr)
    {
        *Errno = localErrno;
    }

    RETURN_IF_FAILED(hr);
    THROW_IF_FAILED(wrapper->git->RegisterInterfaceInGlobal(process.get(), __uuidof(IWSLCProcess), &wrapper->cookie));

    *Process = wrapper.release();
    return S_OK;
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalWslcProcessGetFd(WSLCProcessHandle Process, WSLCProcessFd Fd, SOCKET* Socket)
try
{
    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    *Socket = INVALID_SOCKET;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);

    WSLCFD wslcFd{};
    switch (Fd)
    {
    case WSLCProcessFdStdin:
        wslcFd = WSLCFDStdin;
        break;
    case WSLCProcessFdStdout:
        wslcFd = WSLCFDStdout;
        break;
    case WSLCProcessFdStderr:
        wslcFd = WSLCFDStderr;
        break;
    default:
        return E_INVALIDARG;
    }

    WSLCHandle handle{};
    RETURN_IF_FAILED(wrapper->GetProcess()->GetStdHandle(wslcFd, &handle));

    WI_ASSERT(handle.Type == WSLCHandleTypeSocket);

    // Pass through as SOCKET; COM's system_handle(sh_socket) marshaling already
    // duplicated it into this process.
    *Socket = reinterpret_cast<SOCKET>(handle.Handle.Socket);
    return S_OK;
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalWslcProcessGetExitEvent(WSLCProcessHandle Process, HANDLE* ExitEvent)
try
{
    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, ExitEvent == nullptr);
    *ExitEvent = nullptr;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);
    return wrapper->GetProcess()->GetExitEvent(ExitEvent);
}
CATCH_RETURN()

HRESULT CALLBACK PluginHost::LocalWslcProcessGetExitCode(WSLCProcessHandle Process, int* ExitCode)
try
{
    RETURN_HR_IF(E_INVALIDARG, Process == nullptr);
    RETURN_HR_IF(E_POINTER, ExitCode == nullptr);
    *ExitCode = -1;

    ScopedComInitForCallback coInit;
    RETURN_IF_FAILED(coInit.Result());

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);

    WSLCProcessState state{};
    auto result = wrapper->GetProcess()->GetState(&state, ExitCode);
    if (SUCCEEDED(result) && state != WslcProcessStateExited && state != WslcProcessStateSignalled)
    {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }

    return result;
}
CATCH_RETURN()

void CALLBACK PluginHost::LocalWslcReleaseProcess(WSLCProcessHandle Process)
try
{
    if (Process == nullptr)
    {
        return;
    }

    // Initialize COM before taking ownership: destroying the wrapper releases
    // the IWSLCProcess proxy, which marshals a Release back to wslcsession and
    // needs COM initialized on this thread. coInit is declared first so it
    // outlives the wrapper (reverse destruction order).
    ScopedComInitForCallback coInit;
    THROW_IF_FAILED(coInit.Result());
    std::unique_ptr<WslcProcessWrapper> wrapper{static_cast<WslcProcessWrapper*>(Process)};
}
CATCH_LOG()