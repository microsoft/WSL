/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManager.cpp

Abstract:

    Implementation for WSLASessionManager.

    Sessions run in a per-user COM server process for security isolation.
    CreateComServerAsUser handles process creation/reuse per user.

    Session lifetime is managed through WSLASessionProxy objects:
    - Non-persistent sessions: tracked via weak references, cleaned up when
      all client references are released.
    - Persistent sessions: held via strong references, kept alive until
      explicitly terminated or service shutdown.

    A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ensures that all
    wslasession processes are automatically terminated if wslaservice
    crashes or exits unexpectedly.

--*/

#include "WSLASessionManager.h"
#include "HcsVirtualMachine.h"
#include "wslutil.h"

using wsl::windows::service::wsla::CallingProcessTokenInfo;
using wsl::windows::service::wsla::HcsVirtualMachine;
using wsl::windows::service::wsla::WSLASessionManagerImpl;
using wsl::windows::service::wsla::WSLASessionProxy;
namespace wslutil = wsl::windows::common::wslutil;

WSLASessionManagerImpl::~WSLASessionManagerImpl()
{
    // In case there are still COM references on sessions, signal that the user session is terminating
    // so the sessions are all in a 'terminated' state.
    ForEachSession<void>([](auto& e) { LOG_IF_FAILED(e.Terminate()); });
}

void WSLASessionManagerImpl::CreateSession(const WSLA_SESSION_SETTINGS* Settings, WSLASessionFlags Flags, IWSLASession** WslaSession)
{
    auto tokenInfo = GetCallingProcessTokenInfo();

    std::lock_guard lock(m_wslaSessionsLock);

    // Check for an existing session first.
    auto result = ForEachSession<HRESULT>([&](auto& session) -> std::optional<HRESULT> {
        if (session.DisplayName() == Settings->DisplayName)
        {
            RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), WI_IsFlagClear(Flags, WSLASessionFlagsOpenExisting));

            THROW_IF_FAILED(CheckTokenAccess(session, tokenInfo));
            return session.QueryInterface(__uuidof(IWSLASession), (void**)WslaSession);
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

    // Launch per-user COM server and add it to our job object for crash cleanup.
    auto remoteSession = wslutil::CreateComServerAsUser<WSLASession, IWSLASession>(userToken.get());
    AddSessionProcessToJobObject(remoteSession.get());

    // Initialize the session.
    const auto sessionSettings = CreateSessionSettings(sessionId, creatorPid, Settings);
    THROW_IF_FAILED(remoteSession->Initialize(&sessionSettings, vm.Get()));

    // Create the proxy that wraps the remote session.
    const bool persistent = WI_IsFlagSet(Flags, WSLASessionFlagsPersistent);
    auto proxy = Microsoft::WRL::Make<WSLASessionProxy>(
        sessionId, creatorPid, Settings->DisplayName, std::move(tokenInfo), std::move(remoteSession));

    // Get weak reference for tracking all sessions.
    Microsoft::WRL::ComPtr<IWeakReferenceSource> weakRefSource;
    THROW_IF_FAILED(proxy.As(&weakRefSource));
    Microsoft::WRL::ComPtr<IWeakReference> weakRef;
    THROW_IF_FAILED(weakRefSource->GetWeakReference(&weakRef));
    m_sessions.emplace_back(std::move(weakRef));

    // For persistent sessions, also hold a strong reference to keep them alive.
    if (persistent)
    {
        m_persistentSessions.emplace_back(proxy);
    }

    THROW_IF_FAILED(proxy.CopyTo(__uuidof(IWSLASession), reinterpret_cast<void**>(WslaSession)));
}

void WSLASessionManagerImpl::OpenSession(ULONG Id, IWSLASession** Session)
{
    auto tokenInfo = GetCallingProcessTokenInfo();
    auto result = ForEachSession<HRESULT>([&](auto& e) {
        if (e.GetId() == Id)
        {
            THROW_IF_FAILED(CheckTokenAccess(e, tokenInfo));
            THROW_IF_FAILED(e.QueryInterface(__uuidof(IWSLASession), (void**)Session));
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

    auto result = ForEachSession<HRESULT>([&](auto& e) {
        if (e.DisplayName() == DisplayName)
        {
            THROW_IF_FAILED(CheckTokenAccess(e, tokenInfo));
            THROW_IF_FAILED(e.QueryInterface(__uuidof(IWSLASession), reinterpret_cast<void**>(Session)));
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

    ForEachSession<void>([&](const auto& session) {
        auto& it = sessionInfo.emplace_back(WSLA_SESSION_INFORMATION{.SessionId = session.GetId(), .CreatorPid = session.GetCreatorPid()});

        auto sidString = wslutil::SidToString(session.GetSid());
        wcscpy_s(it.Sid, _countof(it.Sid), sidString.get());
        session.CopyDisplayName(it.DisplayName, _countof(it.DisplayName));
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

void WSLASessionManagerImpl::AddSessionProcessToJobObject(_In_ IWSLASession* Session)
{
    EnsureJobObjectCreated();

    wil::unique_handle process;
    THROW_IF_FAILED(Session->GetProcessHandle(process.put()));

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

    auto elevated = wil::test_token_membership(userToken.get(), SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    return {std::move(tokenInfo), elevated};
}

HRESULT WSLASessionManagerImpl::CheckTokenAccess(const WSLASessionProxy& Session, const CallingProcessTokenInfo& TokenInfo)
{
    // Allow elevated tokens to access all sessions.
    // Otherwise a token can only access sessions from the same SID and elevation status.
    // TODO: Offer proper ACL checks.

    if (TokenInfo.Elevated)
    {
        return S_OK; // Token is elevated, allow access.
    }

    if (!EqualSid(Session.GetSid(), TokenInfo.Info->User.Sid))
    {
        return E_ACCESSDENIED; // Different account, deny access.
    }

    if (!TokenInfo.Elevated && Session.IsTokenElevated())
    {
        return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED); // Non-elevated token trying to access elevated session, deny access.
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