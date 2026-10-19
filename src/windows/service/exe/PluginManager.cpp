// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginManager.h"
#include "LxssUserSessionFactory.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::service::PluginManager;

namespace {

constexpr auto c_pluginPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins";
constexpr BOOL c_pluginAcquireVmLease = FALSE;

std::vector<LPCSTR> TerminateArray(DWORD Count, LPCSTR* Values)
{
    THROW_HR_IF(E_POINTER, Count != 0 && Values == nullptr);
    std::vector<LPCSTR> result;
    if (Count != 0)
    {
        result.assign(Values, Values + Count);
    }
    result.push_back(nullptr);
    return result;
}

} // namespace

class PluginManager::Callback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslPluginHostCallback>
{
public:
    explicit Callback(PluginManager& Owner) : m_owner(Owner)
    {
    }

    IFACEMETHOD(MountFolder)(DWORD Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name) override
    try
    {
        return m_owner.InvokeCallback(Session, [&] {
            const auto session = FindSessionByCookie(Session);
            RETURN_HR_IF(RPC_E_DISCONNECTED, !session);
            return session->MountRootNamespaceFolder(WindowsPath, LinuxPath, ReadOnly, Name);
        });
    }
    CATCH_RETURN()

    IFACEMETHOD(ExecuteBinary)(DWORD Session, LPCSTR Path, DWORD Count, LPCSTR* Arguments, HANDLE* Socket) override
    {
        return ExecuteBinaryInDistribution(Session, nullptr, Path, Count, Arguments, Socket);
    }

    IFACEMETHOD(ExecuteBinaryInDistribution)(DWORD Session, const GUID* Distribution, LPCSTR Path, DWORD Count, LPCSTR* Arguments, HANDLE* Socket) override
    try
    {
        *Socket = nullptr;
        auto arguments = TerminateArray(Count, Arguments);
        wil::unique_socket socket;
        const auto result = m_owner.InvokeCallback(Session, [&] {
            const auto session = FindSessionByCookie(Session);
            RETURN_HR_IF(RPC_E_DISCONNECTED, !session);
            return session->CreateLinuxProcess(Distribution, Path, arguments.data(), socket.put());
        });
        RETURN_IF_FAILED(result);
        *Socket = reinterpret_cast<HANDLE>(socket.release());
        return S_OK;
    }
    CATCH_RETURN()

    IFACEMETHOD(WslcMountFolder)(DWORD Session, LPCWSTR WindowsPath, LPCSTR Mountpoint, BOOL ReadOnly) override
    try
    {
        return m_owner.ResolveWslcSession(Session)->MountWindowsFolder(WindowsPath, Mountpoint, ReadOnly, c_pluginAcquireVmLease);
    }
    CATCH_RETURN()

    IFACEMETHOD(WslcUnmountFolder)(DWORD Session, LPCSTR Mountpoint) override
    try
    {
        return m_owner.ResolveWslcSession(Session)->UnmountWindowsFolder(Mountpoint, c_pluginAcquireVmLease);
    }
    CATCH_RETURN()

    IFACEMETHOD(WslcCreateProcess)(
        DWORD Session, LPCSTR Executable, DWORD Count, LPCSTR* Arguments, DWORD EnvCount, LPCSTR* Environment, IWSLCProcess** Process, int* Errno) override
    try
    {
        *Process = nullptr;
        *Errno = 0;
        WSLCProcessOptions options{};
        options.CommandLine.Count = Count;
        options.CommandLine.Values = Arguments;
        options.Environment.Count = EnvCount;
        options.Environment.Values = Environment;
        options.Flags = WSLCProcessFlagsStdin;
        return m_owner.ResolveWslcSession(Session)->CreateRootNamespaceProcess(Executable, &options, 0, 0, c_pluginAcquireVmLease, Process, Errno);
    }
    CATCH_RETURN()

private:
    PluginManager& m_owner;
};

void PluginManager::LoadPlugins()
{
    auto lock = m_notificationLock.lock_exclusive();
    const auto key = common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_pluginPath, KEY_READ);
    std::set<std::wstring, wsl::shared::string::CaseInsensitiveCompare> loaded;
    for (const auto& [name, type] : common::registry::EnumValues(key.get()))
    {
        if (type != REG_SZ)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Plugin value '%ls' has incorrect type: %lu, skipping", name.c_str(), type);
            continue;
        }
        auto path = common::registry::ReadString(key.get(), nullptr, name.c_str());
        if (!loaded.insert(path).second)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Module '%ls' is already registered, skipping plugin '%ls'", path.c_str(), name.c_str());
            continue;
        }
        auto plugin = std::make_unique<Plugin>();
        plugin->name = name;
        plugin->path = std::move(path);
        m_plugins.push_back(std::move(plugin));
    }
}

void PluginManager::Initialize()
{
    ExecutionContext context(Context::Plugin);
    auto lock = m_notificationLock.lock_exclusive();
    if (m_plugins.empty())
    {
        m_initialized.SetEvent();
        return;
    }

    THROW_IF_FAILED(CoCreateInstance(CLSID_StdGlobalInterfaceTable, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_git)));
    m_job.reset(CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

    for (const auto& plugin : m_plugins)
    {
        const auto result = wil::ResultFromException([&] {
            auto callback = wil::MakeOrThrow<Callback>(*this);
            wil::com_ptr<IWslPluginHost> host;
            THROW_IF_FAILED(CoCreateInstance(CLSID_WslPluginHost, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&host)));
            // Return the marshaled job handle before plugin code can block initialization.
            THROW_IF_FAILED(host->JoinJob(m_job.get()));
            THROW_IF_FAILED(host->Initialize(callback.Get(), plugin->path.c_str(), plugin->name.c_str()));
            plugin->host = std::move(host);
        });

        WSL_LOG_TELEMETRY(
            "PluginLoad",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(plugin->name.c_str(), "Name"),
            TraceLoggingValue(plugin->path.c_str(), "Path"),
            TraceLoggingValue(result, "Result"));
        if (FAILED(result))
        {
            plugin->failure.store(result);
            RecordFailure(*plugin, result);
        }
    }
    m_initialized.SetEvent();
}

void PluginManager::Shutdown()
{
    auto notificationLock = m_notificationLock.lock_exclusive();
    // Hosts must exit before their callbacks and COM proxies are released.
    m_job.reset();
    m_plugins.clear();
    auto lock = m_sessionLock.lock_exclusive();
    for (const auto& [id, cookie] : m_sessions)
    {
        LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(cookie));
    }
    m_sessions.clear();
    m_git.reset();
}

void PluginManager::RecordFailure(const Plugin& Plugin, HRESULT Result)
{
    auto lock = m_errorLock.lock_exclusive();
    if (!m_error)
    {
        m_error.emplace(PluginError{Plugin.name, Result});
    }
}

void PluginManager::ThrowIfFatalPluginError() const
{
    m_initialized.wait();
    ExecutionContext context(Context::Plugin);
    auto lock = m_errorLock.lock_shared();
    if (m_error)
    {
        if (m_error->result == WSL_E_PLUGIN_REQUIRES_UPDATE)
        {
            THROW_HR_WITH_USER_ERROR(m_error->result, wsl::shared::Localization::MessagePluginRequiresUpdate(m_error->name));
        }
        THROW_HR_WITH_USER_ERROR(m_error->result, wsl::shared::Localization::MessageFatalPluginError(m_error->name));
    }
}

bool PluginManager::IsHostDisconnected(HRESULT Result)
{
    return Result == RPC_E_DISCONNECTED || Result == RPC_E_SERVER_DIED || Result == RPC_E_SERVER_DIED_DNE ||
           Result == CO_E_OBJNOTCONNECTED || Result == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           Result == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || Result == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE);
}

void PluginManager::ThrowIfPluginError(HRESULT Result, LPCWSTR Message, LPCWSTR Plugin)
{
    if (FAILED(Result))
    {
        if (Message)
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginErrorWithMessage(Plugin, Message));
        }
        THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginError(Plugin));
    }
    THROW_HR_IF_MSG(E_ILLEGAL_STATE_CHANGE, Message != nullptr, "Plugin '%ls' emitted an error message but returned success", Plugin);
}

std::vector<BYTE> PluginManager::SerializeSid(PSID Sid)
{
    if (!Sid)
    {
        return {};
    }
    THROW_HR_IF(E_INVALIDARG, !IsValidSid(Sid));
    std::vector<BYTE> result(GetLengthSid(Sid));
    THROW_IF_WIN32_BOOL_FALSE(CopySid(static_cast<DWORD>(result.size()), result.data(), Sid));
    return result;
}

void PluginManager::Notify(const Notification& Work, bool Fatal, std::optional<WSLSessionId> Session)
{
    m_initialized.wait();
    auto notificationLock = m_notificationLock.lock_shared();
    if (Fatal)
    {
        ThrowIfFatalPluginError();
    }
    ExecutionContext context(Context::Plugin);
    for (const auto& plugin : m_plugins)
    {
        if (!plugin->host || FAILED(plugin->failure.load()))
        {
            continue;
        }
        auto pump = std::make_shared<PluginCallPump>();
        if (Session)
        {
            auto lock = m_pumpLock.lock_exclusive();
            THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_pumps.emplace(*Session, pump).second);
        }
        auto unregister = wil::scope_exit([&] {
            pump->Stop();
            if (Session)
            {
                auto lock = m_pumpLock.lock_exclusive();
                m_pumps.erase(*Session);
            }
        });

        wil::unique_cotaskmem_string message;
        SlowOperationWatcher watcher{"PluginNotification"};
        const auto result = pump->Run([&] {
            const auto com = wil::CoInitializeEx(COINIT_MULTITHREADED);
            return Work(plugin->host.get(), &message);
        });
        WSL_LOG(
            "PluginNotification",
            TraceLoggingValue(plugin->name.c_str(), "Plugin"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(Fatal, "Fatal"));

        if (IsHostDisconnected(result))
        {
            HRESULT expected = S_OK;
            if (plugin->failure.compare_exchange_strong(expected, result))
            {
                RecordFailure(*plugin, result);
                WSL_LOG_TELEMETRY(
                    "PluginHostCrash",
                    PDT_ProductAndServicePerformance,
                    TraceLoggingValue(plugin->name.c_str(), "Plugin"),
                    TraceLoggingValue(result, "Result"));
            }
        }
        if (Fatal)
        {
            ThrowIfPluginError(result, message.get(), plugin->name.c_str());
        }
        else
        {
            LOG_IF_FAILED_MSG(result, "Error returned by plugin '%ls'", plugin->name.c_str());
        }
    }
}

HRESULT PluginManager::InvokeCallback(WSLSessionId Session, const std::function<HRESULT()>& Work)
{
    for (;;)
    {
        std::shared_ptr<PluginCallPump> pump;
        {
            auto lock = m_pumpLock.lock_shared();
            const auto found = m_pumps.find(Session);
            if (found != m_pumps.end())
            {
                pump = found->second;
            }
        }
        HRESULT result;
        if (pump && pump->Invoke(Work, result))
        {
            return result;
        }
        const auto session = FindSessionByCookie(Session);
        RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

        // Recheck for a pump if a notification acquired the lock before registering it.
        if (session->TryInvokeUnderInstanceLock(std::chrono::milliseconds(25), Work, result))
        {
            return result;
        }
    }
}

void PluginManager::RegisterWslcSession(ULONG SessionId, IWSLCSessionReference* Reference)
{
    auto lock = m_sessionLock.lock_exclusive();
    if (!m_git)
    {
        return;
    }
    DWORD cookie;
    THROW_IF_FAILED(m_git->RegisterInterfaceInGlobal(Reference, __uuidof(IWSLCSessionReference), &cookie));
    auto cleanup = wil::scope_exit([&] { LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(cookie)); });
    THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_sessions.emplace(SessionId, cookie).second);
    cleanup.release();
}

void PluginManager::UnregisterWslcSession(ULONG SessionId)
{
    auto lock = m_sessionLock.lock_exclusive();
    const auto found = m_sessions.find(SessionId);
    if (found != m_sessions.end())
    {
        LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(found->second));
        m_sessions.erase(found);
    }
}

wil::com_ptr<IWSLCSession> PluginManager::ResolveWslcSession(ULONG SessionId)
{
    wil::com_ptr<IWSLCSessionReference> reference;
    {
        auto lock = m_sessionLock.lock_shared();
        const auto found = m_sessions.find(SessionId);
        THROW_HR_IF(WSLC_E_SESSION_NOT_FOUND, found == m_sessions.end());
        THROW_IF_FAILED(m_git->GetInterfaceFromGlobal(found->second, IID_PPV_ARGS(&reference)));
    }
    wil::com_ptr<IWSLCSession> session;
    THROW_IF_FAILED(reference->OpenSession(&session));
    return session;
}

void PluginManager::OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR* message) {
            return host->OnVMStarted(
                Session->SessionId, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), Settings->CustomConfigurationFlags, message);
        },
        true,
        Session->SessionId);
}

void PluginManager::OnVmStopping(const WSLSessionInformation* Session)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnVMStopping(Session->SessionId, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data());
        },
        false,
        Session->SessionId);
}
CATCH_LOG()

void PluginManager::OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR* message) {
            return host->OnDistributionStarted(
                Session->SessionId,
                Session->UserToken,
                static_cast<DWORD>(sid.size()),
                sid.data(),
                &Distribution->Id,
                Distribution->Name,
                Distribution->PidNamespace,
                Distribution->PackageFamilyName,
                Distribution->InitPid,
                Distribution->Flavor,
                Distribution->Version,
                message);
        },
        true,
        Session->SessionId);
}

void PluginManager::OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnDistributionStopping(
                Session->SessionId,
                Session->UserToken,
                static_cast<DWORD>(sid.size()),
                sid.data(),
                &Distribution->Id,
                Distribution->Name,
                Distribution->PidNamespace,
                Distribution->PackageFamilyName,
                Distribution->InitPid,
                Distribution->Flavor,
                Distribution->Version);
        },
        false,
        Session->SessionId);
}
CATCH_LOG()

void PluginManager::OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnDistributionRegistered(
                Session->SessionId,
                Session->UserToken,
                static_cast<DWORD>(sid.size()),
                sid.data(),
                &Distribution->Id,
                Distribution->Name,
                Distribution->PackageFamilyName,
                Distribution->Flavor,
                Distribution->Version);
        },
        false,
        Session->SessionId);
}
CATCH_LOG()

void PluginManager::OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnDistributionUnregistered(
                Session->SessionId,
                Session->UserToken,
                static_cast<DWORD>(sid.size()),
                sid.data(),
                &Distribution->Id,
                Distribution->Name,
                Distribution->PackageFamilyName,
                Distribution->Flavor,
                Distribution->Version);
        },
        false,
        Session->SessionId);
}
CATCH_LOG()

void PluginManager::OnWslcSessionCreated(const WSLCSessionInformation* Session)
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR* message) {
            return host->OnWslcSessionCreated(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), message);
        },
        true);
}

void PluginManager::OnWslcSessionStopping(const WSLCSessionInformation* Session)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcSessionStopping(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data());
        },
        false);
}
CATCH_LOG()

HRESULT PluginManager::OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR* message) {
            return host->OnWslcContainerStarted(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), InspectJson, message);
        },
        true);
    return S_OK;
}
CATCH_RETURN()

void PluginManager::OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcContainerStopping(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), ContainerId);
        },
        false);
}
CATCH_LOG()

void PluginManager::OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcImageCreated(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), InspectJson);
        },
        false);
}
CATCH_LOG()

void PluginManager::OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcImageDeleted(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data(), ImageId);
        },
        false);
}
CATCH_LOG()

void PluginManager::OnWslcVmStarted(const WSLCSessionInformation* Session)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcVmStarted(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data());
        },
        false);
}
CATCH_LOG()

void PluginManager::OnWslcVmStopping(const WSLCSessionInformation* Session)
try
{
    auto sid = SerializeSid(Session->UserSid);
    Notify(
        [&](IWslPluginHost* host, LPWSTR*) {
            return host->OnWslcVmStopping(
                Session->SessionId, Session->DisplayName, Session->ApplicationPid, Session->UserToken, static_cast<DWORD>(sid.size()), sid.data());
        },
        false);
}
CATCH_LOG()
