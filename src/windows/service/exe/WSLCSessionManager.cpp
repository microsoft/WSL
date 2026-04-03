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
#include "wslutil.h"

using wsl::windows::service::wslc::CallingProcessTokenInfo;
using wsl::windows::service::wslc::HcsVirtualMachine;
using wsl::windows::service::wslc::WSLCSessionManagerImpl;
namespace wslutil = wsl::windows::common::wslutil;

WSLCSessionManagerImpl::~WSLCSessionManagerImpl()
{
    // Terminate all sessions on shutdown.
    // Call Terminate() directly rather than going through ForEachSession(),
    // which would needlessly resolve weak references and call GetState().
    // Terminate() already handles the "session is gone" case gracefully.
    std::lock_guard lock(m_wslcSessionsLock);
    for (auto& entry : m_sessions)
    {
        LOG_IF_FAILED(entry.Ref->Terminate());
    }
}

void WSLCSessionManagerImpl::CreateSession(const WSLCSessionSettings* Settings, WSLCSessionFlags Flags, IWSLCSession** WslcSession)
{
    // Ensure that the session display name is non-null and not too long.
    THROW_HR_IF(E_INVALIDARG, Settings->DisplayName == nullptr);
    THROW_HR_IF(E_INVALIDARG, wcslen(Settings->DisplayName) >= std::size(WSLCSessionInformation{}.DisplayName));
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        WI_IsAnyFlagSet(Settings->StorageFlags, ~WSLCSessionStorageFlagsValid),
        "Invalid storage flags: %i",
        Settings->StorageFlags);

    auto tokenInfo = GetCallingProcessTokenInfo();

    std::lock_guard lock(m_wslcSessionsLock);

    // Check for an existing session first.
    auto result = ForEachSession<HRESULT>([&](auto& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (!wsl::shared::string::IsEqual(entry.DisplayName.c_str(), Settings->DisplayName))
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

    HRESULT creationResult = wil::ResultFromException([&]() {
        // Get caller info.
        const auto callerProcess = wslutil::OpenCallingProcess(PROCESS_QUERY_LIMITED_INFORMATION);
        const ULONG sessionId = m_nextSessionId++;
        const DWORD creatorPid = GetProcessId(callerProcess.get());
        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

        // Create the VM in the SYSTEM service (privileged).
        auto vm = Microsoft::WRL::Make<HcsVirtualMachine>(Settings);

        // Launch per-user COM server factory and add it to our job object for crash cleanup.
        auto factory = wslutil::CreateComServerAsUser<IWSLCSessionFactory>(__uuidof(WSLCSessionFactory), userToken.get());
        AddSessionProcessToJobObject(factory.get());

        // Create the session via the factory.
        const auto sessionSettings = CreateSessionSettings(sessionId, creatorPid, Settings);
        wil::com_ptr<IWSLCSession> session;
        wil::com_ptr<IWSLCSessionReference> serviceRef;
        THROW_IF_FAILED(factory->CreateSession(&sessionSettings, vm.Get(), &session, &serviceRef));

        // Track the session via its service ref, along with metadata and security info.
        m_sessions.push_back({std::move(serviceRef), sessionId, creatorPid, Settings->DisplayName, std::move(tokenInfo)});

        // For persistent sessions, also hold a strong reference to keep them alive.
        const bool persistent = WI_IsFlagSet(Flags, WSLCSessionFlagsPersistent);
        if (persistent)
        {
            m_persistentSessions.emplace_back(sessionId, session);
        }

        *WslcSession = session.detach();
    });

    // This telemetry event is used to keep track of session creation performance (via CreationTimeMs) and failure reasons (via Result).

    WSL_LOG_TELEMETRY(
        "WSLCCreateSession",
        PDT_ProductAndServiceUsage,
        TraceLoggingValue(Settings->DisplayName, "Name"),
        TraceLoggingValue(stopWatch.ElapsedMilliseconds(), "CreationTimeMs"),
        TraceLoggingValue(creationResult, "Result"),
        TraceLoggingValue(tokenInfo.Elevated, "Elevated"),
        TraceLoggingValue(static_cast<uint32_t>(Flags), "Flags"));

    THROW_IF_FAILED_MSG(creationResult, "Failed to create session: %ls", Settings->DisplayName);
}

void WSLCSessionManagerImpl::OpenSession(ULONG Id, IWSLCSession** Session)
{
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

    THROW_IF_FAILED_MSG(result.value_or(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)), "Session '%lu' not found", Id);
}

void WSLCSessionManagerImpl::OpenSessionByName(LPCWSTR DisplayName, IWSLCSession** Session)
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    auto result = ForEachSession<HRESULT>([&](auto& entry, const wil::com_ptr<IWSLCSession>& session) noexcept -> std::optional<HRESULT> {
        if (!wsl::shared::string::IsEqual(entry.DisplayName.c_str(), DisplayName))
        {
            return {};
        }

        RETURN_IF_FAILED(CheckTokenAccess(entry, tokenInfo));

        RETURN_IF_FAILED(wil::com_copy_to_nothrow(session, Session));

        return S_OK;
    });

    THROW_IF_FAILED_MSG(result.value_or(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)), "Session '%ls' not found", DisplayName);
}

void WSLCSessionManagerImpl::ListSessions(_Out_ WSLCSessionInformation** Sessions, _Out_ ULONG* SessionsCount)
{
    std::vector<WSLCSessionInformation> sessionInfo;

    ForEachSession<void>([&](auto& entry, const auto&) noexcept {
        try
        {
            wil::unique_hlocal_string sidString;
            THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(entry.Owner.TokenInfo->User.Sid, &sidString));

            auto& it = sessionInfo.emplace_back(WSLCSessionInformation{.SessionId = entry.SessionId, .CreatorPid = entry.CreatorPid});
            wcscpy_s(it.Sid, _countof(it.Sid), sidString.get());
            wcscpy_s(it.DisplayName, _countof(it.DisplayName), entry.DisplayName.c_str());
        }
        CATCH_LOG()
    });

    auto output = wil::make_unique_cotaskmem<WSLCSessionInformation[]>(sessionInfo.size());
    memcpy(output.get(), sessionInfo.data(), sessionInfo.size() * sizeof(WSLCSessionInformation));

    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(sessionInfo.size());
}

void WSLCSessionManagerImpl::GetVersion(_Out_ WSLCVersion* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;
}

WSLCSessionInitSettings WSLCSessionManagerImpl::CreateSessionSettings(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ const WSLCSessionSettings* Settings)
{
    WSLCSessionInitSettings sessionSettings{};
    sessionSettings.SessionId = SessionId;
    sessionSettings.CreatorPid = CreatorPid;
    sessionSettings.DisplayName = Settings->DisplayName;
    sessionSettings.StoragePath = Settings->StoragePath;
    sessionSettings.MaximumStorageSizeMb = Settings->MaximumStorageSizeMb;
    sessionSettings.BootTimeoutMs = Settings->BootTimeoutMs;
    sessionSettings.NetworkingMode = Settings->NetworkingMode;
    sessionSettings.FeatureFlags = Settings->FeatureFlags;
    sessionSettings.RootVhdTypeOverride = Settings->RootVhdTypeOverride;
    sessionSettings.StorageFlags = Settings->StorageFlags;
    return sessionSettings;
}

void WSLCSessionManagerImpl::AddSessionProcessToJobObject(_In_ IWSLCSessionFactory* Factory)
{
    EnsureJobObjectCreated();

    wil::unique_handle process;
    THROW_IF_FAILED(Factory->GetProcessHandle(process.put()));

    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_sessionJobObject.get(), process.get()));
}

void WSLCSessionManagerImpl::EnsureJobObjectCreated()
{
    // Create a job object that will automatically terminate all child processes
    // when the job handle is closed (i.e., when wslservice exits or crashes).
    std::call_once(m_jobObjectInitFlag, [this] {
        m_sessionJobObject.reset(CreateJobObjectW(nullptr, nullptr));
        THROW_LAST_ERROR_IF(!m_sessionJobObject);

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(
            SetInformationJobObject(m_sessionJobObject.get(), JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)));

        WSL_LOG("SessionManagerJobObjectCreated", TraceLoggingLevel(WINEVENT_LEVEL_INFO));
    });
}

CallingProcessTokenInfo WSLCSessionManagerImpl::GetCallingProcessTokenInfo()
{
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());
    auto elevated = wil::test_token_membership(userToken.get(), SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    return {std::move(tokenInfo), elevated};
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

WSLCSessionManager::WSLCSessionManager(WSLCSessionManagerImpl* Impl) : COMImplClass<WSLCSessionManagerImpl>(Impl)
{
}

HRESULT WSLCSessionManager::GetVersion(_Out_ WSLCVersion* Version)
{
    return CallImpl(&WSLCSessionManagerImpl::GetVersion, Version);
}

HRESULT WSLCSessionManager::CreateSession(const WSLCSessionSettings* WslcSessionSettings, WSLCSessionFlags Flags, IWSLCSession** WslcSession)
{
    return CallImpl(&WSLCSessionManagerImpl::CreateSession, WslcSessionSettings, Flags, WslcSession);
}

HRESULT WSLCSessionManager::ListSessions(_Out_ WSLCSessionInformation** Sessions, _Out_ ULONG* SessionsCount)
{
    return CallImpl(&WSLCSessionManagerImpl::ListSessions, Sessions, SessionsCount);
}

HRESULT WSLCSessionManager::OpenSession(_In_ ULONG Id, _Out_ IWSLCSession** Session)
{
    return CallImpl(&WSLCSessionManagerImpl::OpenSession, Id, Session);
}

HRESULT WSLCSessionManager::OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session)
{
    return CallImpl(&WSLCSessionManagerImpl::OpenSessionByName, DisplayName, Session);
}
