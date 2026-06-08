/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginManager.cpp

Abstract:

    This file contains the PluginManager helper class implementation.
    Plugins are loaded in isolated wslpluginhost.exe processes via COM,
    so a crashing plugin cannot take down the WSL service.

--*/

#include "precomp.h"
#include "install.h"
#include "PluginManager.h"
#include "WslPluginApi.h"
#include "WslPluginHost.h"
#include "LxssUserSessionFactory.h"
#include "WSLCSessionManager.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::service::PluginHostCallbackImpl;
using wsl::windows::service::PluginManager;

// Acquire an apartment-local IWslPluginHost proxy for `plugin` (named `host`).
// On a host-process crash, latch a fatal plugin error and `continue` the
// surrounding loop: teardown/notification hooks must not block WSL, but the
// latch makes the next start operation fail fast instead of repeatedly driving
// a dead host. On any other failure to acquire (which would indicate a
// fundamental COM problem, not a plugin-reported issue), log the HRESULT and
// `continue` so a single busted plugin does not break the iteration for the
// others. Use only inside the per-plugin loops in PluginManager hook methods.
#define ACQUIRE_PLUGIN_HOST_OR_CONTINUE(plugin, host, stage) \
    Microsoft::WRL::ComPtr<IWslPluginHost> host; \
    { \
        const HRESULT _acqHr = AcquireHostProxy((plugin), &(host)); \
        if (FAILED(_acqHr)) \
        { \
            if (IsHostCrash(_acqHr)) \
            { \
                LatchHostCrash((plugin), _acqHr, stage "/AcquireHostProxy"); \
            } \
            else \
            { \
                LOG_HR_MSG(_acqHr, "Failed to acquire plugin host proxy for: '%ls'", (plugin).name.c_str()); \
            } \
            continue; \
        } \
    }

// Same as ACQUIRE_PLUGIN_HOST_OR_CONTINUE, but for hook methods that surface a
// plugin error to abort the guarded operation (e.g. OnVmStarted). A host crash
// is fatal: it is latched and thrown as a fatal plugin error, matching the
// pre-refactor behavior where an in-process plugin crash brought down WSL. Any
// other acquisition failure is likewise thrown so it is surfaced exactly like a
// plugin-reported error would be, rather than silently allowing the operation to
// proceed without consulting the plugin.
#define ACQUIRE_PLUGIN_HOST_OR_THROW(plugin, host, stage) \
    Microsoft::WRL::ComPtr<IWslPluginHost> host; \
    { \
        const HRESULT _acqHr = AcquireHostProxy((plugin), &(host)); \
        if (FAILED(_acqHr)) \
        { \
            if (IsHostCrash(_acqHr)) \
            { \
                ThrowHostCrash((plugin), _acqHr, stage "/AcquireHostProxy"); \
            } \
            THROW_HR_MSG(_acqHr, "Failed to acquire plugin host proxy for: '%ls'", (plugin).name.c_str()); \
        } \
    }

constexpr auto c_pluginPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins";

// --- IWslPluginHostCallback implementation (service-side) ---
// These methods handle API calls from the plugin host process.

// Returned to the plugin when a session cookie no longer resolves (the session
// was torn down). Deliberately not an RPC_* status: a plugin that propagates
// this from its hook must not be mistaken for a dead host by IsHostCrash.
constexpr HRESULT c_pluginSessionNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

STDMETHODIMP PluginHostCallbackImpl::MountFolder(_In_ DWORD SessionId, _In_ LPCWSTR WindowsPath, _In_ LPCWSTR LinuxPath, _In_ BOOL ReadOnly, _In_ LPCWSTR Name)
try
{
    RETURN_HR_IF(E_INVALIDARG, WindowsPath == nullptr || LinuxPath == nullptr || Name == nullptr);

    WSL_LOG(
        "PluginCallbackMountFolderBegin",
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(SessionId, "SessionId"));
    const auto session = FindSessionByCookie(SessionId);
    RETURN_HR_IF(c_pluginSessionNotFound, !session);

    auto result = session->MountRootNamespaceFolder(WindowsPath, LinuxPath, ReadOnly, Name);

    WSL_LOG("PluginCallbackMountFolderEnd", TraceLoggingValue(WindowsPath, "WindowsPath"), TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::ExecuteBinary(
    _In_ DWORD SessionId, _In_ LPCSTR Path, _In_ DWORD ArgumentCount, _In_reads_opt_(ArgumentCount) LPCSTR* Arguments, _Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    *Socket = nullptr;
    RETURN_HR_IF(E_INVALIDARG, Path == nullptr);
    RETURN_HR_IF(E_INVALIDARG, ArgumentCount > 0 && Arguments == nullptr);

    WSL_LOG("PluginCallbackExecuteBinaryBegin", TraceLoggingValue(Path, "Path"), TraceLoggingValue(SessionId, "SessionId"));
    const auto session = FindSessionByCookie(SessionId);
    WSL_LOG(
        "PluginCallbackExecuteBinaryFoundSession",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(session != nullptr, "Found"));
    RETURN_HR_IF(c_pluginSessionNotFound, !session);

    // Build NULL-terminated argument array expected by CreateLinuxProcess.
    std::vector<LPCSTR> args;
    if (Arguments != nullptr)
    {
        args.assign(Arguments, Arguments + ArgumentCount);
    }
    args.push_back(nullptr);

    WSL_LOG("PluginCallbackExecuteBinaryCallingCreateProcess", TraceLoggingValue(Path, "Path"));
    wil::unique_socket sock;
    auto result = session->CreateLinuxProcess(nullptr, Path, args.data(), &sock);

    WSL_LOG("PluginCallbackExecuteBinaryEnd", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        // Return socket as HANDLE — COM's system_handle marshaling will
        // duplicate it into the host process automatically.
        *Socket = reinterpret_cast<HANDLE>(sock.release());
    }

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::ExecuteBinaryInDistribution(
    _In_ DWORD SessionId,
    _In_ const GUID* DistributionId,
    _In_ LPCSTR Path,
    _In_ DWORD ArgumentCount,
    _In_reads_opt_(ArgumentCount) LPCSTR* Arguments,
    _Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    *Socket = nullptr;
    RETURN_HR_IF(E_INVALIDARG, DistributionId == nullptr);
    RETURN_HR_IF(E_INVALIDARG, Path == nullptr);
    RETURN_HR_IF(E_INVALIDARG, ArgumentCount > 0 && Arguments == nullptr);

    const auto session = FindSessionByCookie(SessionId);
    RETURN_HR_IF(c_pluginSessionNotFound, !session);

    std::vector<LPCSTR> args;
    if (Arguments != nullptr)
    {
        args.assign(Arguments, Arguments + ArgumentCount);
    }
    args.push_back(nullptr);

    wil::unique_socket sock;
    auto result = session->CreateLinuxProcess(DistributionId, Path, args.data(), &sock);

    WSL_LOG("PluginExecuteBinaryInDistributionCall", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        *Socket = reinterpret_cast<HANDLE>(sock.release());
    }

    return result;
}
CATCH_RETURN();

// --- PluginManager implementation ---

PluginManager::~PluginManager()
{
    // m_plugins should have been cleared by Shutdown() while COM was still
    // initialized. Releasing GIT cookies and the GIT itself here (at global
    // teardown, after CoUninitialize and after the proxy/stub DLL has been
    // unloaded) crashes inside the marshaler. If anything is left here, leak
    // it on purpose rather than dereferencing a torn-down proxy vtable.
    if (!m_plugins.empty())
    {
        LOG_HR_MSG(E_UNEXPECTED, "PluginManager destroyed without Shutdown(); leaking %zu host registrations", m_plugins.size());
        for (auto& e : m_plugins)
        {
            // Drop the cookie without revoking — calling GIT after CoUninitialize crashes.
            e.hostCookie = 0;
            (void)e.callback.Detach();
        }
        m_plugins.clear();
    }
    if (m_git)
    {
        // Same reasoning: leak the GIT reference rather than releasing it after teardown.
        (void)m_git.Detach();
    }

    // WSLC session references are COM proxies too. Shutdown() should have
    // drained them while COM was still initialized; if any survive, detach
    // (leak) them rather than releasing a torn-down proxy.
    if (!m_wslcSessionRefs.empty())
    {
        LOG_HR_MSG(E_UNEXPECTED, "PluginManager destroyed with %zu WSLC session references still registered", m_wslcSessionRefs.size());
        for (auto& [id, ref] : m_wslcSessionRefs)
        {
            (void)ref.detach();
        }
        m_wslcSessionRefs.clear();
    }
    m_jobObject.reset();
}

void PluginManager::Shutdown()
{
    // Must be called while COM is still initialized. Revoking each GIT cookie
    // releases the underlying marshaled IWslPluginHost reference, which causes
    // the wslpluginhost.exe processes to exit; the job object below makes that
    // guaranteed even if a revoke fails.
    if (m_git)
    {
        for (auto& e : m_plugins)
        {
            if (e.hostCookie != 0)
            {
                LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(e.hostCookie));
                e.hostCookie = 0;
            }
        }
        m_git.Reset();
    }
    m_plugins.clear();

    // Release any remaining WSLC session references while COM is still
    // initialized. By this point all sessions should already have been torn
    // down (and thus unregistered), but drain defensively so we never release
    // these proxies from the destructor after CoUninitialize.
    {
        std::lock_guard lock(m_wslcSessionRefLock);
        m_wslcSessionRefs.clear();
    }

    m_jobObject.reset();
}

void PluginManager::LoadPlugins()
{
    ExecutionContext context(Context::Plugin);

    const auto key = common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_pluginPath, KEY_READ);
    const auto values = common::registry::EnumValues(key.get());

    std::set<std::wstring, wsl::shared::string::CaseInsensitiveCompare> loaded;
    for (const auto& e : values)
    {
        if (e.second != REG_SZ)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Plugin value: '%ls' has incorrect type: %lu, skipping", e.first.c_str(), e.second);
            continue;
        }

        auto path = common::registry::ReadString(key.get(), nullptr, e.first.c_str());

        if (!loaded.insert(path).second)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Module '%ls' has already been loaded, skipping plugin '%ls'", path.c_str(), e.first.c_str());
            continue;
        }

        // Record the plugin for deferred activation. The actual COM host process
        // is created in EnsureInitialized(), which runs after the service's COM
        // initialization is complete (CoInitializeSecurity must happen first).
        OutOfProcPlugin plugin{};
        plugin.name = e.first;
        plugin.path = path;
        m_plugins.emplace_back(std::move(plugin));

        // Discovery-only event. The plugin DLL is no longer loaded into
        // wslservice.exe — actual load happens out-of-process via COM
        // activation in EnsureInitialized(). See "PluginLoad" emitted from
        // that path for the real load result.
        WSL_LOG_TELEMETRY(
            "PluginDiscovered",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(e.first.c_str(), "Name"),
            TraceLoggingValue(path.c_str(), "Path"));
    }
}

PluginManager::ScopedComInit::ScopedComInit() : initHr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
{
}

PluginManager::ScopedComInit::~ScopedComInit()
{
    if (SUCCEEDED(initHr))
    {
        ::CoUninitialize();
    }
}

PluginManager::ScopedComInit::ScopedComInit(ScopedComInit&& other) noexcept : initHr(other.initHr)
{
    // Suppress uninit in moved-from instance.
    other.initHr = RPC_E_CHANGED_MODE;
}

HRESULT PluginManager::ScopedComInit::Result() const noexcept
{
    return (initHr == RPC_E_CHANGED_MODE) ? S_OK : initHr;
}

PluginManager::ScopedComInit PluginManager::EnsureInitialized()
{
    // Join the calling thread to the MTA for the duration of the dispatch. The
    // returned guard must outlive any subsequent plugin host calls because
    // those are cross-process COM calls that require an initialized apartment,
    // and so does the GIT-based proxy acquisition below.
    ScopedComInit coInit;
    THROW_IF_FAILED(coInit.Result());

    // Lazily create the process-wide Global Interface Table accessor. The GIT
    // itself is a process-global COM-provided singleton; we just need an
    // in-process accessor to register and look up cookies.
    std::call_once(m_gitOnce, [this]() {
        THROW_IF_FAILED(CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_git)));
    });

    std::call_once(m_initOnce, [this]() {
        for (auto& e : m_plugins)
        {
            auto loadResult = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&]() { LoadPlugin(e); });

            // Canonical "plugin was actually loaded" telemetry — matches the
            // semantics of the pre-refactor PluginLoad event (emitted after
            // the entry point ran). PluginHostActivation below is the more
            // granular event covering the COM activation path specifically.
            WSL_LOG_TELEMETRY(
                "PluginLoad",
                PDT_ProductAndServiceUsage,
                TraceLoggingValue(e.name.c_str(), "Name"),
                TraceLoggingValue(e.path.c_str(), "Path"),
                TraceLoggingValue(loadResult, "Result"));

            WSL_LOG_TELEMETRY(
                "PluginHostActivation",
                PDT_ProductAndServiceUsage,
                TraceLoggingValue(e.name.c_str(), "Name"),
                TraceLoggingValue(e.path.c_str(), "Path"),
                TraceLoggingValue(loadResult, "Result"));

            if (FAILED(loadResult))
            {
                // Any load failure is fatal: the plugin is recorded so that
                // subsequent operations block with a clear error rather than a
                // silently-disabled plugin. This includes host-process crashes
                // and benign-looking COM activation failures (the server is
                // shutting down or its exec failed) — matching the pre-refactor
                // behavior where a plugin that failed to load blocked WSL.
                m_pluginError.emplace(PluginError{e.name, loadResult});
            }
        }
    });

    return coInit;
}

void PluginManager::LoadPlugin(OutOfProcPlugin& plugin)
{
    // One callback object per plugin host so the WSLC process-cookie map is
    // isolated per plugin. When the plugin host process is released, the
    // callback's refcount drops and any unreleased IWSLCProcess refs are freed.
    plugin.callback = Microsoft::WRL::Make<PluginHostCallbackImpl>(*this);
    THROW_IF_NULL_ALLOC(plugin.callback);

    // Activate the plugin host via COM. The LocalServer32 registration causes COM
    // to spawn wslpluginhost.exe automatically.
    Microsoft::WRL::ComPtr<IWslPluginHost> host;
    HRESULT activationHr = CoCreateInstance(CLSID_WslPluginHost, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&host));
    WSL_LOG(
        "PluginHostActivation",
        TraceLoggingValue(plugin.name.c_str(), "Plugin"),
        TraceLoggingValue(plugin.path.c_str(), "Path"),
        TraceLoggingValue(activationHr, "CoCreateInstanceResult"));
    THROW_IF_FAILED_MSG(activationHr, "Failed to create plugin host for: '%ls'", plugin.path.c_str());

    // Create the job object before initializing the host so we can hand it to
    // Initialize. The host assigns itself to the job before running any plugin
    // code, so any child processes the plugin spawns inherit the job and are
    // killed when the service exits. Job assignment is fatal in the host: a host
    // that isn't in the job would escape the kill-on-close guarantee, so a
    // failure surfaces here (alongside activation and plugin entry-point errors)
    // and the host process exits when its proxy is released on unwind.
    // system_handle(sh_job) marshaling duplicates the job handle into the host
    // for the duration of the Initialize call.
    EnsureJobObjectCreated();

    THROW_IF_FAILED_MSG(
        host->Initialize(plugin.callback.Get(), m_jobObject.get(), plugin.path.c_str(), plugin.name.c_str()),
        "Plugin host failed to initialize: '%ls'",
        plugin.path.c_str());

    // Stash the IWslPluginHost in the Global Interface Table. The proxy returned
    // by CoCreateInstance is bound to the apartment of this thread (MTA, since
    // EnsureInitialized() joined us to the MTA). Hook dispatch can arrive on
    // threads in any apartment — in particular NTA-on-MTA, which is what wslservice's
    // RPC dispatcher uses when the incoming call is via a WinRT-style interface
    // (e.g. IWSLCSession). MTA-bound proxies are NOT callable from NTA, so storing
    // the raw proxy here and reusing it cross-apartment fails with RPC_E_WRONG_THREAD.
    // Storing in the GIT and re-unmarshaling per call yields an apartment-local
    // proxy that works from any apartment.
    DWORD cookie = 0;
    THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(host.Get(), __uuidof(IWslPluginHost), &cookie));

    auto revokeOnFailure = wil::scope_exit([&] {
        if (cookie != 0)
        {
            LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(cookie));
        }
    });

    // Drop the raw proxy — future access goes through the GIT to get an
    // apartment-local proxy. The GIT keeps the underlying marshaled stream
    // alive, so the host process stays running.
    host.Reset();

    plugin.hostCookie = cookie;
    cookie = 0;
    revokeOnFailure.release();
}

HRESULT PluginManager::AcquireHostProxy(const OutOfProcPlugin& plugin, _COM_Outptr_ IWslPluginHost** host)
{
    *host = nullptr;
    if (plugin.hostCookie == 0 || !m_git)
    {
        return E_NOT_VALID_STATE;
    }
    return m_git->GetInterfaceFromGlobal(plugin.hostCookie, __uuidof(IWslPluginHost), reinterpret_cast<void**>(host));
}

void PluginManager::EnsureJobObjectCreated()
{
    std::call_once(m_jobObjectOnce, [this]() {
        m_jobObject.reset(CreateJobObjectW(nullptr, nullptr));
        THROW_LAST_ERROR_IF(!m_jobObject);

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(m_jobObject.get(), JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)));
    });
}

std::vector<BYTE> PluginManager::SerializeSid(PSID Sid)
{
    const DWORD sidLength = GetLengthSid(Sid);
    std::vector<BYTE> buffer(sidLength);
    CopySid(sidLength, buffer.data(), Sid);
    return buffer;
}

void PluginManager::OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG("PluginOnVmStartedCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

        ACQUIRE_PLUGIN_HOST_OR_THROW(e, host, "OnVmStarted");

        wil::unique_cotaskmem_string errorMessage;
        SlowOperationWatcher slowOperation{"PluginOnVmStarted"};
        WSL_LOG("PluginOnVmStartedBeginRpc", TraceLoggingValue(e.name.c_str(), "Plugin"));
        HRESULT hr = host->OnVMStarted(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            static_cast<DWORD>(Settings->CustomConfigurationFlags),
            &errorMessage);
        WSL_LOG("PluginOnVmStartedEndRpc", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(hr, "Result"));

        if (IsHostCrash(hr))
        {
            ThrowHostCrash(e, hr, "OnVmStarted");
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
}

void PluginManager::OnVmStopping(const WSLSessionInformation* Session)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG("PluginOnVmStoppingCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnVmStopping");

        const auto result = host->OnVMStopping(Session->SessionId, Session->UserToken, static_cast<DWORD>(sidData.size()), sidData.data());

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnVmStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistroStartedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        ACQUIRE_PLUGIN_HOST_OR_THROW(e, host, "OnDistributionStarted");

        wil::unique_cotaskmem_string errorMessage;
        SlowOperationWatcher slowOperation{"PluginOnDistributionStarted"};
        HRESULT hr = host->OnDistributionStarted(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PidNamespace,
            Distribution->PackageFamilyName,
            Distribution->InitPid,
            Distribution->Flavor,
            Distribution->Version,
            &errorMessage);

        if (IsHostCrash(hr))
        {
            ThrowHostCrash(e, hr, "OnDistributionStarted");
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
}

void PluginManager::OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistroStoppingCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnDistributionStopping");

        const auto result = host->OnDistributionStopping(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PidNamespace,
            Distribution->PackageFamilyName,
            Distribution->InitPid,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnDistributionStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistributionRegisteredCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnDistributionRegistered");

        const auto result = host->OnDistributionRegistered(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PackageFamilyName,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnDistributionRegistered");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistributionUnregisteredCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnDistributionUnregistered");

        const auto result = host->OnDistributionUnregistered(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PackageFamilyName,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnDistributionUnregistered");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::ThrowIfPluginError(HRESULT Result, LPWSTR ErrorMessage, WSLSessionId Session, LPCWSTR Plugin)
{
    if (FAILED(Result))
    {
        if (ErrorMessage != nullptr && ErrorMessage[0] != L'\0')
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginErrorWithMessage(Plugin, ErrorMessage));
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginError(Plugin));
        }
    }
    else if (ErrorMessage != nullptr && ErrorMessage[0] != L'\0')
    {
        THROW_HR_MSG(E_ILLEGAL_STATE_CHANGE, "Plugin '%ls' emitted an error message but returned success", Plugin);
    }
}

bool PluginManager::IsHostCrash(HRESULT hr)
{
    // Each of these unambiguously indicates the COM server process has gone
    // away. RPC_E_CALL_REJECTED is deliberately excluded: it means a busy
    // server rejected the call, not that the server died — treating it as a
    // crash would silently disable the plugin for the rest of the session.
    switch (hr)
    {
    case RPC_E_DISCONNECTED:
    case RPC_E_SERVER_DIED:
    case RPC_E_SERVER_DIED_DNE:
    case CO_E_OBJNOTCONNECTED:
    case HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE):
    case HRESULT_FROM_WIN32(RPC_S_CALL_FAILED):
    case HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE):
        return true;
    default:
        return false;
    }
}

void PluginManager::LatchHostCrash(OutOfProcPlugin& plugin, HRESULT result, const char* stage)
{
    LOG_HR_MSG(result, "Plugin host crashed at %hs for: '%ls'", stage, plugin.name.c_str());

    // Fire telemetry only on first observation per plugin: a dead plugin will
    // hit this path on every subsequent VM/distro lifecycle event, and we
    // don't want to flood the telemetry channel with duplicates. Any WSLC
    // processes the dead host created are released automatically by COM when
    // the host process exits, so there is nothing to drain here.
    if (!plugin.crashTelemetryFired.exchange(true))
    {
        WSL_LOG_TELEMETRY(
            "PluginHostCrash",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(plugin.name.c_str(), "Name"),
            TraceLoggingValue(plugin.path.c_str(), "Path"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(stage, "Stage"));
    }

    // Latch a fatal plugin error so subsequent operations fail fast with a
    // single consistent message instead of repeatedly driving a dead host that
    // is never re-activated for this service lifetime. The first crash wins; a
    // load-time failure already recorded in m_pluginError is left untouched.
    auto lock = m_pluginErrorLock.lock_exclusive();
    if (!m_pluginError.has_value())
    {
        m_pluginError.emplace(PluginError{plugin.name, result});
    }
}

void PluginManager::ThrowHostCrash(OutOfProcPlugin& plugin, HRESULT result, const char* stage)
{
    // Record the crash (telemetry + fatal latch), then throw a fatal plugin
    // error so the guarded start/veto operation (VM/distro/session/container
    // creation) is aborted. The HRESULT is whichever RPC/CO_E_* code COM
    // surfaced for the dead host; it is reported the same way a plugin-returned
    // fatal error would be.
    LatchHostCrash(plugin, result, stage);
    THROW_HR_WITH_USER_ERROR(result, wsl::shared::Localization::MessageFatalPluginError(plugin.name.c_str()));
}

void PluginManager::ThrowIfFatalPluginError()
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    // m_pluginError can be set at load time (single-threaded, under m_initOnce)
    // or latched at runtime from a hook on any RPC thread when a host crash is
    // observed, so read it under the lock and act on a local copy.
    std::optional<PluginError> error;
    {
        auto lock = m_pluginErrorLock.lock_shared();
        error = m_pluginError;
    }

    if (!error.has_value())
    {
        return;
    }
    else if (error->error == WSL_E_PLUGIN_REQUIRES_UPDATE)
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_PLUGIN_REQUIRES_UPDATE, wsl::shared::Localization::MessagePluginRequiresUpdate(error->plugin));
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(error->error, wsl::shared::Localization::MessageFatalPluginError(error->plugin));
    }
}

void PluginManager::RegisterWslcSession(ULONG SessionId, IWSLCSessionReference* Reference)
{
    THROW_HR_IF(E_POINTER, Reference == nullptr);

    std::lock_guard lock(m_wslcSessionRefLock);
    m_wslcSessionRefs[SessionId] = Reference;
}

void PluginManager::UnregisterWslcSession(ULONG SessionId)
{
    std::lock_guard lock(m_wslcSessionRefLock);
    m_wslcSessionRefs.erase(SessionId);
}

wil::com_ptr<IWSLCSession> PluginManager::ResolveWslcSession(ULONG SessionId)
{
    wil::com_ptr<IWSLCSessionReference> reference;
    {
        std::lock_guard lock(m_wslcSessionRefLock);
        auto it = m_wslcSessionRefs.find(SessionId);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_wslcSessionRefs.end());
        reference = it->second;
    }

    // OpenSession() is called OUTSIDE m_wslcSessionRefLock: the reference is a
    // weak handle, so opening it can fail or block and must not hold the lock.
    wil::com_ptr<IWSLCSession> session;
    const auto hr = reference->OpenSession(&session);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), FAILED(hr) || session == nullptr);

    return session;
}

void PluginManager::OnWslcSessionCreated(const WSLCSessionInformation* Session)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcSessionCreatedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"),
            TraceLoggingValue(Session->DisplayName, "DisplayName"));

        ACQUIRE_PLUGIN_HOST_OR_THROW(e, host, "OnWslcSessionCreated");

        wil::unique_cotaskmem_string errorMessage;
        HRESULT hr = host->OnWslcSessionCreated(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &errorMessage);

        if (IsHostCrash(hr))
        {
            ThrowHostCrash(e, hr, "OnWslcSessionCreated");
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
}

void PluginManager::OnWslcSessionStopping(const WSLCSessionInformation* Session)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcSessionStoppingCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnWslcSessionStopping");

        const auto result = host->OnWslcSessionStopping(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data());

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnWslcSessionStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

HRESULT PluginManager::OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcContainerStartedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"));

        ACQUIRE_PLUGIN_HOST_OR_THROW(e, host, "OnWslcContainerStarted");

        // Failure here aborts the container creation. Surface the first error.
        wil::unique_cotaskmem_string errorMessage;
        const HRESULT hr = host->OnWslcContainerStarted(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            InspectJson,
            &errorMessage);

        if (IsHostCrash(hr))
        {
            ThrowHostCrash(e, hr, "OnWslcContainerStarted");
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
    return S_OK;
}
CATCH_RETURN()

void PluginManager::OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcContainerStoppingCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"),
            TraceLoggingValue(ContainerId, "ContainerId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnWslcContainerStopping");

        const auto result = host->OnWslcContainerStopping(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            ContainerId);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnWslcContainerStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcImageCreatedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnWslcImageCreated");

        const auto result = host->OnWslcImageCreated(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            InspectJson);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnWslcImageCreated");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (e.hostCookie == 0)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnWslcImageDeletedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->SessionId, "SessionId"),
            TraceLoggingValue(ImageId, "ImageId"));

        ACQUIRE_PLUGIN_HOST_OR_CONTINUE(e, host, "OnWslcImageDeleted");

        const auto result = host->OnWslcImageDeleted(
            Session->SessionId,
            Session->DisplayName,
            Session->ApplicationPid,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            ImageId);

        if (IsHostCrash(result))
        {
            LatchHostCrash(e, result, "OnWslcImageDeleted");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

// --- IWslPluginHostCallback WSLC implementations (service-side) ---

STDMETHODIMP PluginHostCallbackImpl::WslcMountFolder(_In_ DWORD SessionId, _In_ LPCWSTR WindowsPath, _In_ LPCSTR Mountpoint, _In_ BOOL ReadOnly)
try
{
    // TODO: Add logic to validate that the mountpoint isn't in use by another plugin.
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || Mountpoint == nullptr);

    auto session = m_owner.ResolveWslcSession(SessionId);

    auto result = session->MountWindowsFolder(WindowsPath, Mountpoint, ReadOnly);

    WSL_LOG(
        "WslcPluginMountFolderCall",
        TraceLoggingValue(SessionId, "SessionId"),
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(Mountpoint, "Mountpoint"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::WslcUnmountFolder(_In_ DWORD SessionId, _In_ LPCSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    auto session = m_owner.ResolveWslcSession(SessionId);
    auto result = session->UnmountWindowsFolder(Mountpoint);

    WSL_LOG(
        "WslcPluginUnmountFolderCall",
        TraceLoggingValue(SessionId, "SessionId"),
        TraceLoggingValue(Mountpoint, "Mountpoint"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::WslcCreateProcess(
    _In_ DWORD SessionId,
    _In_ LPCSTR Executable,
    _In_ DWORD ArgumentCount,
    _In_reads_opt_(ArgumentCount) LPCSTR* Arguments,
    _In_ DWORD EnvCount,
    _In_reads_opt_(EnvCount) LPCSTR* Environment,
    _COM_Outptr_ IWSLCProcess** Process,
    _Out_ int* Errno)
try
{
    RETURN_HR_IF(E_POINTER, Executable == nullptr || Process == nullptr || Errno == nullptr);
    *Process = nullptr;
    *Errno = 0;
    RETURN_HR_IF(E_INVALIDARG, (ArgumentCount > 0 && Arguments == nullptr) || (EnvCount > 0 && Environment == nullptr));

    auto session = m_owner.ResolveWslcSession(SessionId);

    // Build NULL-terminated argument/env arrays expected by CreateRootNamespaceProcess.
    std::vector<LPCSTR> argsTerminated;
    if (Arguments != nullptr)
    {
        argsTerminated.assign(Arguments, Arguments + ArgumentCount);
    }
    argsTerminated.push_back(nullptr);

    std::vector<LPCSTR> envTerminated;
    if (Environment != nullptr)
    {
        envTerminated.assign(Environment, Environment + EnvCount);
        envTerminated.push_back(nullptr);
    }

    WSLCProcessOptions options{};
    options.CommandLine.Values = argsTerminated.data();
    options.CommandLine.Count = ArgumentCount;
    if (!envTerminated.empty())
    {
        options.Environment.Values = envTerminated.data();
        options.Environment.Count = EnvCount;
    }
    options.Flags = WSLCProcessFlagsStdin;

    wil::com_ptr<IWSLCProcess> process;
    int errnoValue = 0;
    auto result = session->CreateRootNamespaceProcess(Executable, &options, 0, 0, &process, &errnoValue);
    *Errno = errnoValue;

    if (FAILED(result))
    {
        WSL_LOG(
            "WslcPluginCreateProcessCall",
            TraceLoggingValue(SessionId, "SessionId"),
            TraceLoggingValue(Executable, "Executable"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(errnoValue, "Errno"));
        return result;
    }

    // Hand the IWSLCProcess back to the host. COM marshals it across the host
    // boundary; the host then calls it directly (GetStdHandle/GetExitEvent/
    // GetState) and owns its lifetime, so the service keeps no per-process state.
    *Process = process.detach();

    WSL_LOG(
        "WslcPluginCreateProcessCall",
        TraceLoggingValue(SessionId, "SessionId"),
        TraceLoggingValue(Executable, "Executable"),
        TraceLoggingValue(S_OK, "Result"));

    return S_OK;
}
CATCH_RETURN();
