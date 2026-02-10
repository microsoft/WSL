/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManager.h

Abstract:

    Definition for WSLASessionManager.

    Session Lifetime Management:
    ----------------------------
    Sessions are created in per-user COM server processes via IWSLASessionFactory.
    The SYSTEM service holds IWSLASessionReference objects that contain weak
    references to the actual sessions.

    - Non-persistent sessions: Lifetime is tied to client COM references.
      When all clients release their IWSLASession references, the session is
      terminated and the weak reference in IWSLASessionReference returns NULL.

    - Persistent sessions: The service holds an additional strong IWSLASession
      reference to keep the session alive until explicitly terminated or service
      shutdown.

    The IWSLASessionReference allows the service to:
    - Check if a session is still alive (OpenSession fails if session is gone)
    - Get session metadata for enumeration without holding strong refs
    - Terminate sessions when requested by elevated callers

--*/

#pragma once
#include "wslaservice.h"
#include "COMImplClass.h"
#include "wslutil.h"
#include <atomic>
#include <algorithm>
#include <vector>
#include <mutex>
#include <string>

namespace wslutil = wsl::windows::common::wslutil;

namespace wsl::windows::service::wsla {

struct CallingProcessTokenInfo
{
    wil::unique_hlocal_string SidString;
    bool Elevated;
};

class WSLASessionManagerImpl
{
public:
    NON_COPYABLE(WSLASessionManagerImpl);
    NON_MOVABLE(WSLASessionManagerImpl);

    WSLASessionManagerImpl() = default;
    ~WSLASessionManagerImpl();

    void GetVersion(_Out_ WSLA_VERSION* Version);
    void CreateSession(const WSLA_SESSION_SETTINGS* WslaSessionSettings, WSLASessionFlags Flags, IWSLASession** WslaSession);
    void ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount);
    void OpenSession(_In_ ULONG Id, _Out_ IWSLASession** Session);
    void OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session);

private:
    // Iterates over all sessions, cleaning up released sessions.
    // The routine receives an IWSLASessionReference& and can return an optional<T> to stop iteration.
    template <typename T>
    inline auto ForEachSession(const auto& Routine)
    {
        std::lock_guard lock(m_wslaSessionsLock);

        using TResult = std::conditional_t<std::is_same_v<T, void>, nullptr_t, std::optional<T>>;
        TResult result{};

        auto each = [&](const wil::com_ptr<IWSLASessionReference>& SessionRef) {
            // Try to open the session via the service ref.
            // Fails with ERROR_OBJECT_NO_LONGER_EXISTS if released,
            // ERROR_INVALID_STATE if terminated, or RPC error if per-user process is dead.
            wil::com_ptr<IWSLASession> lockedSession;
            if (FAILED_LOG(SessionRef->OpenSession(&lockedSession)))
            {
                // Session is gone, drop the persistent reference if any.
                ULONG refId = 0;
                if (SUCCEEDED_LOG(SessionRef->GetId(&refId)))
                {
                    auto remove = std::ranges::remove_if(m_persistentSessions, [&](const auto& e) {
                        ULONG sessionId = 0;
                        return SUCCEEDED(e->GetId(&sessionId)) && refId == sessionId;
                    });
                    m_persistentSessions.erase(remove.begin(), remove.end());
                }
                return true; // Remove from tracking
            }

            if constexpr (std::is_same_v<T, void>)
            {
                Routine(*SessionRef, lockedSession);
            }
            else
            {
                if (!result.has_value())
                {
                    result = Routine(*SessionRef, lockedSession);
                }
            }

            return false; // Keep in tracking
        };

        auto remove = std::ranges::remove_if(m_sessions, each);
        m_sessions.erase(remove.begin(), remove.end());

        if constexpr (std::is_same_v<T, void>)
        {
            return;
        }
        else
        {
            return result;
        }
    }

    void AddSessionProcessToJobObject(_In_ IWSLASessionFactory* Factory);
    WSLA_SESSION_INIT_SETTINGS CreateSessionSettings(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ const WSLA_SESSION_SETTINGS* Settings);
    void EnsureJobObjectCreated();
    static CallingProcessTokenInfo GetCallingProcessTokenInfo();
    static HRESULT CheckTokenAccess(IWSLASessionReference* SessionRef, const CallingProcessTokenInfo& TokenInfo);

    std::atomic<ULONG> m_nextSessionId{1};
    std::recursive_mutex m_wslaSessionsLock;

    // Job object that automatically terminates all child COM server processes
    // when this service exits or crashes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
    std::once_flag m_jobObjectInitFlag;
    wil::unique_handle m_sessionJobObject;

    // All sessions tracked via IWSLASessionReference (which holds weak refs).
    // Sessions are automatically cleaned up when the underlying session is released.
    std::vector<wil::com_ptr<IWSLASessionReference>> m_sessions;

    // Strong references to persistent sessions to keep them alive.
    std::vector<wil::com_ptr<IWSLASession>> m_persistentSessions;
};
} // namespace wsl::windows::service::wsla

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLASessionManager
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASessionManager, IFastRundown>,
      public wsl::windows::service::wsla::COMImplClass<wsl::windows::service::wsla::WSLASessionManagerImpl>
{
public:
    NON_COPYABLE(WSLASessionManager);
    NON_MOVABLE(WSLASessionManager);

    WSLASessionManager(wsl::windows::service::wsla::WSLASessionManagerImpl* Impl);

    IFACEMETHOD(GetVersion)(_Out_ WSLA_VERSION* Version) override;
    IFACEMETHOD(CreateSession)(const WSLA_SESSION_SETTINGS* WslaSessionSettings, WSLASessionFlags Flags, IWSLASession** WslaSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLASession** Session) override;
    IFACEMETHOD(OpenSessionByName)(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session) override;
};
