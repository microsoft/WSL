/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionManager.cpp

Abstract:

    Implementation for WSLCSessionManager.

    Sessions run in a per-user COM server process for security isolation.
    The SYSTEM service creates sessions via IWSLCSessionFactory which returns
    both the session interface (for clients) and an IWSLCSessionReference
    (for the service to track sessions via weak references).

    Session lifetime:
    - Non-persistent sessions: tracked via IWSLCSessionReference which holds
      weak references. Sessions are cleaned up when all client refs are released.
    - Persistent sessions: the service holds an additional strong IWSLCSession
      reference to keep them alive until explicitly terminated.

    A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ensures that all
    per-user COM server processes are automatically terminated if wslservice
    crashes or exits unexpectedly.

--*/

#include "WSLCSessionManager.h"
#include "HcsVirtualMachine.h"
#include "WSLCUserSettings.h"
#include "WSLCSessionDefaults.h"
#include "WSLCPluginNotifier.h"
#include "PluginManager.h"
#include "ExecutionContext.h"
#include "helpers.hpp"
#include "wslutil.h"
#include "filesystem.hpp"
#include "APICompat.h"
#include "Localization.h"

extern wsl::windows::service::PluginManager g_pluginManager;

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::service::wslc::CallingProcessTokenInfo;
using wsl::windows::service::wslc::HcsVirtualMachine;
using wsl::windows::service::wslc::WSLCPluginNotifier;
using wsl::windows::service::wslc::WSLCSessionManagerImpl;
using wsl::windows::service::wslc::WSLCVirtualMachineFactory;
namespace wslutil = wsl::windows::common::wslutil;
namespace apicompat = wsl::windows::common::apicompat;
namespace settings = wsl::windows::wslc::settings;

namespace {

std::atomic<wsl::windows::service::wslc::WSLCSessionManagerImpl*> g_managerInstance{nullptr};

// Session settings built server-side from the caller's settings.yaml.
struct SessionSettings
{
    std::wstring DisplayName;
    std::wstring StoragePath;
    WSLCSessionSettings Settings{};

    NON_COPYABLE(SessionSettings);
    NON_MOVABLE(SessionSettings);

    // Load user settings under impersonation.
    static settings::UserSettings LoadUserSettings(HANDLE UserToken)
    {
        auto localAppData = wsl::windows::common::filesystem::GetLocalAppDataPath(UserToken);
        auto runAsUser = wil::impersonate_token(UserToken);
        return settings::UserSettings(localAppData / L"wslc");
    }

    // Get default memory size. Half of available memory.
    static uint32_t DefaultMemoryMb()
    {
        MEMORYSTATUSEX memInfo{sizeof(MEMORYSTATUSEX)};
        THROW_IF_WIN32_BOOL_FALSE(GlobalMemoryStatusEx(&memInfo));
        return static_cast<uint32_t>(memInfo.ullTotalPhys / (2 * _1MB));
    }

    // Default session: name and storage path determined from caller's token.
    static std::unique_ptr<SessionSettings> Default(HANDLE UserToken, const std::wstring& ResolvedName)
    {
        auto userSettings = LoadUserSettings(UserToken);

        auto configuredStorageBase = userSettings.Get<settings::Setting::SessionStoragePath>();
        const bool customConfigured = !configuredStorageBase.empty();
        const std::filesystem::path defaultBase = wsl::windows::common::filesystem::GetLocalAppDataPath(UserToken);
        const std::filesystem::path storageBase =
            customConfigured ? std::filesystem::path(wsl::shared::string::MultiByteToWide(configuredStorageBase)) : defaultBase;

        const auto storageDir = storageBase / wsl::windows::wslc::DefaultStorageSubPath / ResolvedName;

        // wslcsession emits the custom-location warning when it actually creates the VHD, so the notice
        // fires once at creation without a service-side callback that could stall CreateSession.
        const auto storageFlags = customConfigured ? WSLCSessionStorageFlagsWarnCustomLocation : WSLCSessionStorageFlagsNone;

        return std::unique_ptr<SessionSettings>(new SessionSettings(std::wstring(ResolvedName), storageDir.wstring(), storageFlags, userSettings));
    }

    // Custom session: caller provides name and storage path.
    static SessionSettings Custom(HANDLE UserToken, LPCWSTR Name, LPCWSTR Path, WSLCSessionStorageFlags StorageFlags = WSLCSessionStorageFlagsNone)
    {
        auto userSettings = LoadUserSettings(UserToken);
        return SessionSettings(Name, Path, StorageFlags, userSettings);
    }

private:
    SessionSettings(std::wstring name, std::wstring path, WSLCSessionStorageFlags storageFlags, const settings::UserSettings& userSettings) :
        DisplayName(std::move(name)), StoragePath(std::move(path))
    {
        Settings.DisplayName = DisplayName.c_str();
        Settings.StoragePath = StoragePath.c_str();
        auto cpuCount = userSettings.Get<settings::Setting::SessionCpuCount>();
        Settings.CpuCount = cpuCount > 0 ? cpuCount : wsl::windows::common::wslutil::GetLogicalProcessorCount();
        auto memoryMb = userSettings.Get<settings::Setting::SessionMemoryMb>();
        Settings.MemoryMb = memoryMb > 0 ? memoryMb : SessionSettings::DefaultMemoryMb();
        Settings.MaximumStorageSizeMb = userSettings.Get<settings::Setting::SessionStorageSizeMb>();
        Settings.BootTimeoutMs = wsl::windows::wslc::DefaultBootTimeoutMs;
        Settings.NetworkingMode = userSettings.Get<settings::Setting::SessionNetworkingMode>();

        // TODO: Add a config setting to opt-out of GPU support.
        Settings.FeatureFlags = WslcFeatureFlagsGPU;
        WI_SetFlagIf(Settings.FeatureFlags, WslcFeatureFlagsDnsTunneling, userSettings.Get<settings::Setting::SessionDnsTunneling>());
        WI_SetFlagIf(
            Settings.FeatureFlags,
            WslcFeatureFlagsVirtioFs,
            userSettings.Get<settings::Setting::SessionHostFileShareMode>() == settings::HostFileShareMode::VirtioFs);
        WI_SetFlagIf(
            Settings.FeatureFlags,
            WslcFeatureFlagsPortRelayWslRelay,
            userSettings.Get<settings::Setting::SessionPortRelay>() == settings::PortRelayType::WslRelay);
        Settings.StorageFlags = storageFlags;
    }
};

} // namespace

WSLCSessionManagerImpl::WSLCSessionManagerImpl()
{
    g_managerInstance.store(this);
}

WSLCSessionManagerImpl::~WSLCSessionManagerImpl()
{
    g_managerInstance.store(nullptr);

    // Terminate all sessions on shutdown.
    // Call Terminate() directly rather than going through ForEachSession(),
    // which would needlessly resolve weak references and call GetState().
    // Terminate() already handles the "session is gone" case gracefully.
    std::lock_guard lock(m_wslcSessionsLock);
    for (auto& entry : m_sessions)
    {
        NotifySessionStoppingLockHeld(entry);
        LOG_IF_FAILED(entry.Ref->Terminate());
    }
}

void WSLCSessionManagerImpl::NotifySessionStoppingLockHeld(SessionEntry& entry) noexcept
try
{
    if (entry.StoppingNotified)
    {
        return;
    }

    entry.StoppingNotified = true;
    WSLCSessionInformation info{};
    info.SessionId = static_cast<WSLCSessionId>(entry.SessionId);
    info.DisplayName = entry.DisplayName.c_str();
    info.ApplicationPid = entry.CreatorPid;
    info.UserToken = entry.UserToken.get();
    info.UserSid = entry.UserSid.data();
    g_pluginManager.OnWslcSessionStopping(&info);
}
CATCH_LOG()

void WSLCSessionManagerImpl::CreateSession(
    _In_ const WSLCSessionSettings* Settings, _In_ WSLCSessionFlags Flags, _In_opt_ IWarningCallback* WarningCallback, _Out_ IWSLCSession** WslcSession)
{
    THROW_HR_IF_NULL(E_POINTER, WslcSession);

    auto tokenInfo = GetCallingProcessTokenInfo();
    const auto callerToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    // Resolve display name upfront (for both default and custom sessions).
    std::wstring resolvedDisplayName;
    if (Settings == nullptr)
    {
        // Default session: name determined from token, qualified with username.
        resolvedDisplayName = ResolveDefaultSessionName(tokenInfo);
        Flags = WSLCSessionFlagsOpenExisting | WSLCSessionFlagsPersistent;
    }
    else
    {
        THROW_HR_IF(WSLC_E_INVALID_SESSION_NAME, Settings->DisplayName == nullptr || wcslen(Settings->DisplayName) == 0);
        THROW_HR_IF(E_INVALIDARG, Settings->StoragePath != nullptr && wcslen(Settings->StoragePath) == 0);
        THROW_HR_IF(WSLC_E_INVALID_SESSION_NAME, wcslen(Settings->DisplayName) >= std::size(WSLCSessionListEntry{}.DisplayName));
        THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCSessionFlagsValid), "Invalid session flags: 0x%x", Flags);
        THROW_HR_IF_MSG(
            E_INVALIDARG, WI_IsAnyFlagSet(Settings->FeatureFlags, ~WSLCFeatureFlagsValid), "Invalid feature flags: 0x%x", Settings->FeatureFlags);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            WI_IsAnyFlagSet(Settings->StorageFlags, ~WSLCSessionStorageFlagsValid),
            "Invalid storage flags: %i",
            Settings->StorageFlags);

        // Reserved names can only be assigned server-side via null Settings.
        THROW_HR_IF(WSLC_E_SESSION_RESERVED, IsReservedSessionName(Settings->DisplayName));

        resolvedDisplayName = Settings->DisplayName;
    }

    std::lock_guard lock(m_wslcSessionsLock);

    // Check for an existing session first.
    auto result = ForEachSession<HRESULT>([&](auto& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (!wsl::shared::string::IsEqual(entry.DisplayName.c_str(), resolvedDisplayName.c_str()))
        {
            return {};
        }

        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), WI_IsFlagClear(Flags, WSLCSessionFlagsOpenExisting));

        RETURN_IF_FAILED(CheckTokenAccess(entry, tokenInfo));

        RETURN_IF_FAILED(wil::com_copy_to_nothrow(session, WslcSession));

        return S_OK;
    });

    if (result.has_value())
    {
        THROW_IF_FAILED(result.value());
        return; // Existing session was opened.
    }

    wslutil::StopWatch stopWatch;

    // Initialize settings for the default session.
    std::unique_ptr<SessionSettings> defaultSettings;
    if (Settings == nullptr)
    {
        defaultSettings = SessionSettings::Default(callerToken.get(), resolvedDisplayName);
        Settings = &defaultSettings->Settings;
    }

    std::wstring callerFileName;

    HRESULT creationResult = wil::ResultFromException([&]() {
        // Get caller info.
        const auto callerProcess = wslutil::OpenCallingProcess(PROCESS_QUERY_LIMITED_INFORMATION);
        const ULONG sessionId = m_nextSessionId++;
        const DWORD creatorPid = GetProcessId(callerProcess.get());

        // Query the full image path of the calling process and extract just the file name.
        std::wstring callerFilePath;
        if (SUCCEEDED_LOG(wil::QueryFullProcessImageNameW<std::wstring>(callerProcess.get(), 0, callerFilePath)))
        {
            callerFileName = std::filesystem::path(callerFilePath).filename().wstring();
        }

        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

        // Capture a duplicated user token + raw SID so PluginManager can build
        // WSLCSessionInformation later (e.g. on shutdown) without re-impersonating.
        // The token is shared between the SessionEntry and the WSLCPluginNotifier.
        wil::unique_handle dupToken;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            userToken.get(), TOKEN_QUERY | TOKEN_DUPLICATE, nullptr, SecurityImpersonation, TokenImpersonation, &dupToken));
        wil::shared_handle sharedToken{dupToken.release()};

        const DWORD sidLen = GetLengthSid(tokenInfo.TokenInfo->User.Sid);
        std::vector<BYTE> storedSid(sidLen);
        THROW_IF_WIN32_BOOL_FALSE(CopySid(sidLen, storedSid.data(), tokenInfo.TokenInfo->User.Sid));

        // Build the plugin notifier service-side. Lifetime tracked via the SessionEntry.
        Microsoft::WRL::ComPtr<IWSLCPluginNotifier> notifier;
        notifier = wil::MakeOrThrow<WSLCPluginNotifier>(
            g_pluginManager, sessionId, creatorPid, std::wstring(resolvedDisplayName), wil::shared_handle(sharedToken), std::vector<BYTE>(storedSid));

        // Create the VM factory in the SYSTEM service (privileged). The per-user session
        // uses it to create the VM. Funneling VM creation through a factory lets the session
        // own when VMs are created, rather than having one handed to it up front.
        auto vmFactory = Microsoft::WRL::Make<WSLCVirtualMachineFactory>(Settings);

        // Launch per-user COM server factory and add it to a fresh per-session job object for crash cleanup.
        auto factory = wslutil::CreateComServerAsUser<IWSLCSessionFactory>(__uuidof(WSLCSessionFactory), userToken.get());
        wil::unique_handle sessionJob = CreateSessionProcessJob(factory.get());

        const auto sessionSettings = CreateSessionSettings(sessionId, callerFileName.c_str(), Settings, resolvedDisplayName.c_str());
        wil::com_ptr<IWSLCSession> session;
        wil::com_ptr<IWSLCSessionReference> serviceRef;
        const auto factoryHr =
            factory->CreateSession(&sessionSettings, vmFactory.Get(), notifier.Get(), WarningCallback, &session, &serviceRef);
        if (FAILED(factoryHr))
        {
            if (auto comError = wslutil::GetCOMErrorInfo(); comError && comError->Message)
            {
                THROW_HR_WITH_USER_ERROR(factoryHr, comError->Message.get());
            }

            THROW_HR(factoryHr);
        }

        // Track the session via its service ref, along with metadata and security info.
        m_sessions.push_back(SessionEntry{
            std::move(serviceRef), sessionId, creatorPid, resolvedDisplayName, std::move(tokenInfo), notifier, false, sharedToken, std::move(storedSid), std::move(sessionJob)});

        // For persistent sessions, also hold a strong reference to keep them alive.
        const bool persistent = WI_IsFlagSet(Flags, WSLCSessionFlagsPersistent);
        if (persistent)
        {
            m_persistentSessions.emplace_back(sessionId, session);
        }

        // Notify plugins that the session was created. A failure here aborts session creation.
        try
        {
            auto& entry = m_sessions.back();
            WSLCSessionInformation info{};
            info.SessionId = static_cast<WSLCSessionId>(entry.SessionId);
            info.DisplayName = entry.DisplayName.c_str();
            info.ApplicationPid = entry.CreatorPid;
            info.UserToken = entry.UserToken.get();
            info.UserSid = entry.UserSid.data();
            g_pluginManager.OnWslcSessionCreated(&info);
        }
        catch (...)
        {
            const auto error = wil::ResultFromCaughtException();

            // Plugin rejected the session: tear it down before propagating.
            m_sessions.back().StoppingNotified = true; // Don't fire stopping for a session that never started successfully.
            LOG_IF_FAILED(m_sessions.back().Ref->Terminate());
            m_sessions.pop_back();

            auto remove = std::ranges::remove_if(m_persistentSessions, [&](const auto& e) { return e.first == sessionId; });
            m_persistentSessions.erase(remove.begin(), remove.end());

            THROW_HR(error);
        }

        *WslcSession = session.detach();
    });

    // This telemetry event is used to keep track of session creation performance (via CreationTimeMs) and failure reasons (via Result).
    WSL_LOG(
        "WSLCCreateSession",
        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA),
        TraceLoggingValue(resolvedDisplayName.c_str(), "Name"),
        TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"),
        TraceLoggingValue(stopWatch.ElapsedMilliseconds(), "CreationTimeMs"),
        TraceLoggingValue(creationResult, "Result"),
        TraceLoggingValue(tokenInfo.Elevated, "Elevated"),
        TraceLoggingValue(static_cast<uint32_t>(Flags), "Flags"),
        TraceLoggingValue(callerFileName.c_str(), "CallerFileName"),
        TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    THROW_IF_FAILED_MSG(creationResult, "Failed to create session: %ls", resolvedDisplayName.c_str());
}

void WSLCSessionManagerImpl::OpenSession(ULONG Id, IWSLCSession** Session)
{
    THROW_HR_IF_NULL(E_POINTER, Session);

    auto tokenInfo = GetCallingProcessTokenInfo();
    auto result = ForEachSession<HRESULT>([&](auto& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (entry.SessionId != Id)
        {
            return {};
        }

        RETURN_IF_FAILED(CheckTokenAccess(entry, tokenInfo));

        RETURN_IF_FAILED(wil::com_copy_to_nothrow(session, Session));

        return S_OK;
    });

    THROW_IF_FAILED_MSG(result.value_or(WSLC_E_SESSION_NOT_FOUND), "Session '%lu' not found", Id);
}

void WSLCSessionManagerImpl::OpenSessionByName(LPCWSTR DisplayName, IWSLCSession** Session)
{
    THROW_HR_IF_NULL(E_POINTER, Session);

    auto tokenInfo = GetCallingProcessTokenInfo();

    // Null name = default session, resolved from caller's token + username.
    std::wstring resolvedName;
    if (DisplayName == nullptr)
    {
        resolvedName = ResolveDefaultSessionName(tokenInfo);
        DisplayName = resolvedName.c_str();
    }

    auto result = ForEachSession<HRESULT>([&](auto& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (!wsl::shared::string::IsEqual(entry.DisplayName.c_str(), DisplayName))
        {
            return {};
        }

        RETURN_IF_FAILED(CheckTokenAccess(entry, tokenInfo));

        RETURN_IF_FAILED(wil::com_copy_to_nothrow(session, Session));

        return S_OK;
    });

    THROW_HR_WITH_USER_ERROR_IF(
        WSLC_E_SESSION_NOT_FOUND, wsl::shared::Localization::MessageWslcSessionNotFound(DisplayName), !result.has_value());

    THROW_IF_FAILED_MSG(result.value(), "Failed to open session '%ls'", DisplayName);
}

void WSLCSessionManagerImpl::ListSessions(_Out_ WSLCSessionListEntry** Sessions, _Out_ ULONG* SessionsCount)
{
    THROW_HR_IF_NULL(E_POINTER, Sessions);
    THROW_HR_IF_NULL(E_POINTER, SessionsCount);

    std::vector<WSLCSessionListEntry> sessionInfo;

    ForEachSession<void>([&](auto& entry, const auto&) noexcept {
        try
        {
            wil::unique_hlocal_string sidString;
            THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(entry.Owner.TokenInfo->User.Sid, &sidString));

            auto& it = sessionInfo.emplace_back(WSLCSessionListEntry{.SessionId = entry.SessionId, .CreatorPid = entry.CreatorPid});
            wcscpy_s(it.Sid, _countof(it.Sid), sidString.get());
            wcscpy_s(it.DisplayName, _countof(it.DisplayName), entry.DisplayName.c_str());
        }
        CATCH_LOG()
    });

    auto output = wil::make_unique_cotaskmem<WSLCSessionListEntry[]>(sessionInfo.size());
    memcpy(output.get(), sessionInfo.data(), sessionInfo.size() * sizeof(WSLCSessionListEntry));

    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(sessionInfo.size());
}

void WSLCSessionManagerImpl::EnterSession(
    _In_ LPCWSTR DisplayName, _In_ LPCWSTR StoragePath, _In_opt_ IWarningCallback* WarningCallback, _Out_ IWSLCSession** WslcSession)
{
    THROW_HR_IF(E_POINTER, DisplayName == nullptr || StoragePath == nullptr);
    THROW_HR_IF(E_INVALIDARG, DisplayName[0] == L'\0' || StoragePath[0] == L'\0');

    const auto callerToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    auto sessionSettings = SessionSettings::Custom(callerToken.get(), DisplayName, StoragePath, WSLCSessionStorageFlagsNoCreate);
    CreateSession(&sessionSettings.Settings, WSLCSessionFlagsNone, WarningCallback, WslcSession);
}

WSLCSessionInitSettings WSLCSessionManagerImpl::CreateSessionSettings(
    _In_ ULONG SessionId, _In_ LPCWSTR CreatorProcessName, _In_ const WSLCSessionSettings* Settings, _In_ LPCWSTR ResolvedDisplayName)
{
    WSLCSessionInitSettings sessionSettings{};
    sessionSettings.SessionId = SessionId;
    sessionSettings.CreatorProcessName = CreatorProcessName;
    sessionSettings.DisplayName = ResolvedDisplayName;
    sessionSettings.StoragePath = Settings->StoragePath;
    sessionSettings.MaximumStorageSizeMb = Settings->MaximumStorageSizeMb;
    sessionSettings.BootTimeoutMs = Settings->BootTimeoutMs;
    sessionSettings.NetworkingMode = Settings->NetworkingMode;
    sessionSettings.FeatureFlags = Settings->FeatureFlags;
    sessionSettings.RootVhdTypeOverride = Settings->RootVhdTypeOverride;
    sessionSettings.StorageFlags = Settings->StorageFlags;
    sessionSettings.SwapSizeMb = Settings->MemoryMb;
    return sessionSettings;
}

wil::unique_handle WSLCSessionManagerImpl::CreateSessionProcessJob(_In_ IWSLCSessionFactory* Factory)
{
    // Use a fresh job per session; reusing one fails intermittently with
    // ERROR_ACCESS_DENIED once it's assigned to a process the system put in another job.
    wil::unique_handle jobObject = wsl::windows::common::helpers::CreateKillOnCloseJob();

    wil::unique_handle process;
    THROW_IF_FAILED(Factory->GetProcessHandle(process.put()));

    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(jobObject.get(), process.get()));

    return jobObject;
}

CallingProcessTokenInfo WSLCSessionManagerImpl::GetCallingProcessTokenInfo()
{
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());
    auto elevated = wil::test_token_membership(userToken.get(), SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    return {std::move(tokenInfo), elevated};
}

std::wstring WSLCSessionManagerImpl::ResolveDefaultSessionName(const CallingProcessTokenInfo& TokenInfo)
{
    // Look up the username from the caller's SID so each user gets their own
    // default session (e.g. "wslc-cli-alice", "wslc-cli-admin-bob").
    wchar_t username[256 + 1] = {};
    DWORD usernameLen = ARRAYSIZE(username);
    wchar_t domain[MAX_PATH] = {};
    DWORD domainLen = ARRAYSIZE(domain);
    SID_NAME_USE sidType;
    THROW_IF_WIN32_BOOL_FALSE(LookupAccountSidW(nullptr, TokenInfo.TokenInfo->User.Sid, username, &usernameLen, domain, &domainLen, &sidType));

    auto baseName = TokenInfo.Elevated ? wsl::windows::wslc::DefaultAdminSessionName : wsl::windows::wslc::DefaultSessionName;
    return std::format(L"{}-{}", baseName, username);
}

bool WSLCSessionManagerImpl::IsReservedSessionName(LPCWSTR Name)
{
    // Block any name that is exactly "wslc-cli" or starts with "wslc-cli-",
    // which covers the admin variant and all per-user resolved names.
    constexpr std::wstring_view prefix{wsl::windows::wslc::DefaultSessionName};
    std::wstring_view name{Name};
    if (name.size() < prefix.size())
    {
        return false;
    }

    if (!wsl::shared::string::IsEqual(name.substr(0, prefix.size()), prefix, true))
    {
        return false;
    }

    return name.size() == prefix.size() || name[prefix.size()] == L'-';
}

HRESULT WSLCSessionManagerImpl::CheckTokenAccess(const SessionEntry& Entry, const CallingProcessTokenInfo& TokenInfo)
{
    // Allow elevated tokens to access all sessions.
    // Otherwise a token can only access sessions from the same SID and elevation status.
    // TODO: Offer proper ACL checks.

    if (TokenInfo.Elevated)
    {
        return S_OK; // Token is elevated, allow access.
    }

    RETURN_HR_IF(E_ACCESSDENIED, !EqualSid(Entry.Owner.TokenInfo->User.Sid, TokenInfo.TokenInfo->User.Sid)); // Different account, deny access.

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED), Entry.Owner.Elevated); // Non-elevated token trying to access elevated session, deny access.

    return S_OK;
}

WSLCSessionManager::WSLCSessionManager(WSLCSessionManagerImpl* Impl)
{
    Initialize(Impl);
}

HRESULT WSLCSessionManager::GetVersion(_Out_ WSLCVersion* Version)
try
{
    RETURN_HR_IF(E_POINTER, Version == nullptr);

    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSessionManager::IsClientVersionSupported(_In_ const WSLCCompatVersion* ClientVersion, _Out_ BOOL* IsSupported)
try
{
    RETURN_HR_IF(E_POINTER, ClientVersion == nullptr || IsSupported == nullptr);

    WSL_LOG(
        "ClientVersionCheck",
        TraceLoggingValue(ClientVersion->Major, "Major"),
        TraceLoggingValue(ClientVersion->Minor, "Minor"),
        TraceLoggingValue(ClientVersion->Revision, "Revision"));

    constexpr std::tuple<uint32_t, uint32_t, uint32_t> c_minClientVersion{2, 9, 0};

    const std::tuple<uint32_t, uint32_t, uint32_t> clientVersion{ClientVersion->Major, ClientVersion->Minor, ClientVersion->Revision};

    // For now set 2.9.0 as the floor version. Also support if the client version exactly matches ours to cover builds before 2.9.0.
    *IsSupported = (clientVersion >= c_minClientVersion || wsl::shared::PackageVersion == clientVersion);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSessionManager::CreateSession(
    const WSLCSessionSettings* WslcSessionSettings, WSLCSessionFlags Flags, IWarningCallback* WarningCallback, IWSLCSession** WslcSession)
try
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCSessionManagerImpl::CreateSession, WslcSessionSettings, Flags, WarningCallback, WslcSession);
}
CATCH_RETURN();

HRESULT WSLCSessionManager::EnterSession(_In_ LPCWSTR DisplayName, _In_ LPCWSTR StoragePath, IWarningCallback* WarningCallback, IWSLCSession** WslcSession)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCSessionManagerImpl::EnterSession, DisplayName, StoragePath, WarningCallback, WslcSession);
}

HRESULT WSLCSessionManager::ListSessions(_Out_ WSLCSessionListEntry** Sessions, _Out_ ULONG* SessionsCount)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCSessionManagerImpl::ListSessions, Sessions, SessionsCount);
}

HRESULT WSLCSessionManager::OpenSession(_In_ ULONG Id, _Out_ IWSLCSession** Session)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCSessionManagerImpl::OpenSession, Id, Session);
}

HRESULT WSLCSessionManager::OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCSessionManagerImpl::OpenSessionByName, DisplayName, Session);
}

HRESULT WSLCSessionManager::InterfaceSupportsErrorInfo(_In_ REFIID riid)
{
    return riid == __uuidof(IWSLCSessionManager) ? S_OK : S_FALSE;
}

HRESULT WSLCSessionManager::GetVersion(_Out_ WSLCCompatVersion* Version)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Version);

    WSLCVersion version{};
    RETURN_IF_FAILED(GetVersion(&version));

    *Version = apicompat::Convert(version);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCSessionManager::CreateSession(
    const WSLCCompatSessionSettings* Settings, WSLCSessionFlags Flags, IWSLCCompatWarningCallback* WarningCallback, IWSLCCompatSession** Session)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Session);
    *Session = nullptr;

    const auto warning = apicompat::Convert(WarningCallback);

    Microsoft::WRL::ComPtr<IWSLCSession> session;
    if (Settings == nullptr)
    {
        RETURN_IF_FAILED(CreateSession(static_cast<const WSLCSessionSettings*>(nullptr), Flags, warning.Get(), &session));
    }
    else
    {
        const auto settings = apicompat::Convert(*Settings);
        RETURN_IF_FAILED(CreateSession(settings.Get(), Flags, warning.Get(), &session));
    }

    RETURN_HR_IF_NULL(E_UNEXPECTED, session);

    return session.CopyTo(Session);
}
CATCH_RETURN();

namespace wsl::windows::service::wslc {

WSLCSessionManagerImpl* WSLCSessionManagerImpl::Instance() noexcept
{
    return g_managerInstance.load();
}

wil::com_ptr<IWSLCSession> WSLCSessionManagerImpl::FindSession(ULONG Id)
{
    wil::com_ptr<IWSLCSession> result;

    ForEachSession<HRESULT>([&](SessionEntry& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (entry.SessionId != Id)
        {
            return std::nullopt;
        }

        result = session;
        return S_OK;
    });

    THROW_HR_IF_MSG(WSLC_E_SESSION_NOT_FOUND, !result, "WSLC session %lu not found", Id);
    return result;
}

} // namespace wsl::windows::service::wslc
