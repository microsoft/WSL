/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManager.cpp

Abstract:

    Implementation for WSLASessionManager.

    Sessions run in a per-user COM server process for security isolation.
    The SYSTEM service creates sessions via IWSLASessionFactory which returns
    both the session interface (for clients) and an IWSLASessionReference
    (for the service to track sessions via weak references).

    Session lifetime:
    - Non-persistent sessions: tracked via IWSLASessionReference which holds
      weak references. Sessions are cleaned up when all client refs are released.
    - Persistent sessions: the service holds an additional strong IWSLASession
      reference to keep them alive until explicitly terminated.

    A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ensures that all
    per-user COM server processes are automatically terminated if wslaservice
    crashes or exits unexpectedly.

--*/

#include "WSLASessionManager.h"
#include "WSLASessionFactory.h"
#include "HcsVirtualMachine.h"
#include "wslutil.h"

using wsl::windows::service::wsla::CallingProcessTokenInfo;
using wsl::windows::service::wsla::HcsVirtualMachine;
using wsl::windows::service::wsla::WSLASessionManagerImpl;
namespace wslutil = wsl::windows::common::wslutil;

WSLASessionManagerImpl::~WSLASessionManagerImpl()
{
    // Terminate all sessions on shutdown.
    ForEachSession<void>([](auto& sessionRef, const auto&) { LOG_IF_FAILED(sessionRef.Terminate()); });
}

void WSLASessionManagerImpl::CreateSession(const WSLA_SESSION_SETTINGS* Settings, WSLASessionFlags Flags, IWSLASession** WslaSession)
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    std::lock_guard lock(m_wslaSessionsLock);

    // Check for an existing session first.
    auto result = ForEachSession<HRESULT>([&](auto& sessionRef, const wil::com_ptr<IWSLASession>& session) -> std::optional<HRESULT> {
        wil::unique_cotaskmem_string displayName;
        THROW_IF_FAILED(sessionRef.GetDisplayName(&displayName));

        if (wsl::shared::string::IsEqual(displayName.get(), Settings->DisplayName))
        {
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), WI_IsFlagClear(Flags, WSLASessionFlagsOpenExisting));

            THROW_IF_FAILED(CheckTokenAccess(&sessionRef, tokenInfo));

            session.copy_to(WslaSession);
            return S_OK;
        }

        return std::optional<HRESULT>{};
    });

    if (result.has_value())
    {
        THROW_IF_FAILED(result.value());
        return; // Existing session was opened.
    }

    // Get caller info.
    const auto callerProcess = wslutil::OpenCallingProcess(PROCESS_QUERY_LIMITED_INFORMATION);
    const ULONG sessionId = m_nextSessionId++;
    const DWORD creatorPid = GetProcessId(callerProcess.get());
    const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    // Create the VM in the SYSTEM service (privileged).
    auto vm = Microsoft::WRL::Make<HcsVirtualMachine>(Settings);

    // Launch per-user COM server factory and add it to our job object for crash cleanup.
    auto factory = wslutil::CreateComServerAsUser<WSLASessionFactory, IWSLASessionFactory>(userToken.get());
    AddSessionProcessToJobObject(factory.get());

    // Create the session via the factory.
    const auto sessionSettings = CreateSessionSettings(sessionId, creatorPid, Settings);
    wil::com_ptr<IWSLASession> session;
    wil::com_ptr<IWSLASessionReference> serviceRef;
    THROW_IF_FAILED(factory->CreateSession(&sessionSettings, vm.Get(), &session, &serviceRef));

    // Track the session via its service ref.
    m_sessions.emplace_back(std::move(serviceRef));

    // For persistent sessions, also hold a strong reference to keep them alive.
    const bool persistent = WI_IsFlagSet(Flags, WSLASessionFlagsPersistent);
    if (persistent)
    {
        m_persistentSessions.emplace_back(session);
    }

    *WslaSession = session.detach();
}

void WSLASessionManagerImpl::OpenSession(ULONG Id, IWSLASession** Session)
{
    auto tokenInfo = GetCallingProcessTokenInfo();
    auto result = ForEachSession<HRESULT>([&](auto& sessionRef, const wil::com_ptr<IWSLASession>& session) {
        ULONG sessionId = 0;
        THROW_IF_FAILED(sessionRef.GetId(&sessionId));

        if (sessionId == Id)
        {
            THROW_IF_FAILED(CheckTokenAccess(&sessionRef, tokenInfo));
            session.copy_to(Session);
            return std::make_optional(S_OK);
        }
        else
        {
            return std::optional<HRESULT>{};
        }
    });

    THROW_IF_FAILED_MSG(result.value_or(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)), "Session '%lu' not found", Id);
}

void WSLASessionManagerImpl::OpenSessionByName(LPCWSTR DisplayName, IWSLASession** Session)
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    auto result = ForEachSession<HRESULT>([&](auto& sessionRef, const wil::com_ptr<IWSLASession>& session) {
        wil::unique_cotaskmem_string name;
        THROW_IF_FAILED(sessionRef.GetDisplayName(&name));

        if (wsl::shared::string::IsEqual(name.get(), DisplayName))
        {
            THROW_IF_FAILED(CheckTokenAccess(&sessionRef, tokenInfo));
            session.copy_to(Session);
            return std::make_optional(S_OK);
        }
        else
        {
            return std::optional<HRESULT>{};
        }
    });

    THROW_IF_FAILED_MSG(result.value_or(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)), "Session '%ls' not found", DisplayName);
}

void WSLASessionManagerImpl::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    std::vector<WSLA_SESSION_INFORMATION> sessionInfo;

    ForEachSession<void>([&](auto& sessionRef, const auto&) {
        ULONG sessionId = 0;
        DWORD creatorPid = 0;
        wil::unique_cotaskmem_string displayName;
        wil::unique_cotaskmem_string sidString;

        THROW_IF_FAILED(sessionRef.GetId(&sessionId));
        THROW_IF_FAILED(sessionRef.GetCreatorPid(&creatorPid));
        THROW_IF_FAILED(sessionRef.GetDisplayName(&displayName));
        THROW_IF_FAILED(sessionRef.GetSid(&sidString));

        auto& it = sessionInfo.emplace_back(WSLA_SESSION_INFORMATION{.SessionId = sessionId, .CreatorPid = creatorPid});
        wcscpy_s(it.Sid, _countof(it.Sid), sidString.get());
        wcscpy_s(it.DisplayName, _countof(it.DisplayName), displayName.get());
    });

    auto output = wil::make_unique_cotaskmem<WSLA_SESSION_INFORMATION[]>(sessionInfo.size());
    for (size_t i = 0; i < sessionInfo.size(); i++)
    {
        memcpy(&output[i], &sessionInfo[i], sizeof(WSLA_SESSION_INFORMATION));
    }

    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(sessionInfo.size());
}

void WSLASessionManagerImpl::GetVersion(_Out_ WSLA_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;
}

WSLA_SESSION_INIT_SETTINGS WSLASessionManagerImpl::CreateSessionSettings(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ const WSLA_SESSION_SETTINGS* Settings)
{
    WSLA_SESSION_INIT_SETTINGS sessionSettings{};
    sessionSettings.SessionId = SessionId;
    sessionSettings.CreatorPid = CreatorPid;
    sessionSettings.DisplayName = Settings->DisplayName;
    sessionSettings.StoragePath = Settings->StoragePath;
    sessionSettings.MaximumStorageSizeMb = Settings->MaximumStorageSizeMb;
    sessionSettings.BootTimeoutMs = Settings->BootTimeoutMs;
    sessionSettings.NetworkingMode = Settings->NetworkingMode;
    sessionSettings.FeatureFlags = Settings->FeatureFlags;
    sessionSettings.RootVhdTypeOverride = Settings->RootVhdTypeOverride;
    return sessionSettings;
}

void WSLASessionManagerImpl::AddSessionProcessToJobObject(_In_ IWSLASessionFactory* Factory)
{
    EnsureJobObjectCreated();

    wil::unique_handle process;
    THROW_IF_FAILED(Factory->GetProcessHandle(process.put()));

    THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(m_sessionJobObject.get(), process.get()));
}

void WSLASessionManagerImpl::EnsureJobObjectCreated()
{
    // Create a job object that will automatically terminate all child processes
    // when the job handle is closed (i.e., when wslaservice exits or crashes).
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

CallingProcessTokenInfo WSLASessionManagerImpl::GetCallingProcessTokenInfo()
{
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());

    wil::unique_cotaskmem_string sidString;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(tokenInfo->User.Sid, &sidString));

    auto elevated = wil::test_token_membership(userToken.get(), SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    return {std::move(sidString), elevated};
}

HRESULT WSLASessionManagerImpl::CheckTokenAccess(IWSLASessionReference* SessionRef, const CallingProcessTokenInfo& TokenInfo)
{
    // Allow elevated tokens to access all sessions.
    // Otherwise a token can only access sessions from the same SID and elevation status.
    // TODO: Offer proper ACL checks.

    if (TokenInfo.Elevated)
    {
        return S_OK; // Token is elevated, allow access.
    }

    wil::unique_cotaskmem_string sessionSidString;
    RETURN_IF_FAILED(SessionRef->GetSid(&sessionSidString));

    RETURN_HR_IF(E_ACCESSDENIED, !wsl::shared::string::IsEqual(sessionSidString.get(), TokenInfo.SidString.get())); // Different account, deny access.

    if (!TokenInfo.Elevated)
    {
        BOOL sessionElevated = FALSE;
        RETURN_IF_FAILED(SessionRef->IsElevated(&sessionElevated));

        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED), sessionElevated); // Non-elevated token trying to access elevated session, deny access.
    }

    return S_OK;
}

WSLASessionManager::WSLASessionManager(WSLASessionManagerImpl* Impl) : COMImplClass<WSLASessionManagerImpl>(Impl)
{
}

HRESULT WSLASessionManager::GetVersion(_Out_ WSLA_VERSION* Version)
{
    return CallImpl(&WSLASessionManagerImpl::GetVersion, Version);
}

HRESULT WSLASessionManager::CreateSession(const WSLA_SESSION_SETTINGS* WslaSessionSettings, WSLASessionFlags Flags, IWSLASession** WslaSession)
{
    return CallImpl(&WSLASessionManagerImpl::CreateSession, WslaSessionSettings, Flags, WslaSession);
}

HRESULT WSLASessionManager::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    return CallImpl(&WSLASessionManagerImpl::ListSessions, Sessions, SessionsCount);
}

HRESULT WSLASessionManager::OpenSession(_In_ ULONG Id, _Out_ IWSLASession** Session)
{
    return CallImpl(&WSLASessionManagerImpl::OpenSession, Id, Session);
}

HRESULT WSLASessionManager::OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session)
{
    return CallImpl(&WSLASessionManagerImpl::OpenSessionByName, DisplayName, Session);
}
