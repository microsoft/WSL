/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionManager.h

Abstract:

    Definition for WSLCSessionManager.

    Session Lifetime Management:
    ----------------------------
    Sessions are created in per-user COM server processes via IWSLCSessionFactory.
    The SYSTEM service holds IWSLCSessionReference objects that contain weak
    references to the actual sessions.

    - Non-persistent sessions: Lifetime is tied to client COM references.
      When all clients release their IWSLCSession references, the session is
      terminated and the weak reference in IWSLCSessionReference returns NULL.

    - Persistent sessions: The service holds an additional strong IWSLCSession
      reference to keep the session alive until explicitly terminated or service
      shutdown.

    The IWSLCSessionReference allows the service to:
    - Check if a session is still alive (OpenSession fails if session is gone)
    - Terminate sessions when requested by elevated callers

--*/

#pragma once
#include "wslc.h"
#include "COMImplClass.h"
#include "wslutil.h"
#include <atomic>
#include <algorithm>
#include <string>
#include <vector>
#include <mutex>
#include <type_traits>

namespace wslutil = wsl::windows::common::wslutil;

namespace wsl::windows::service::wslc {

struct CallingProcessTokenInfo
{
    wil::unique_tokeninfo_ptr<TOKEN_USER> TokenInfo;
    bool Elevated;
};

// Metadata for a tracked session, stored service-side at creation time.
// Security info is stored here (not queried from the per-user process) to prevent spoofing.
struct SessionEntry
{
    wil::com_ptr<IWSLCSessionReference> Ref;
    ULONG SessionId = 0;
    DWORD CreatorPid = 0;
    std::wstring DisplayName;
    CallingProcessTokenInfo Owner;
};

class WSLCSessionManagerImpl
{
public:
    NON_COPYABLE(WSLCSessionManagerImpl);
    NON_MOVABLE(WSLCSessionManagerImpl);

    WSLCSessionManagerImpl() = default;
    ~WSLCSessionManagerImpl();

    void GetVersion(_Out_ WSLCVersion* Version);
    void CreateSession(const WSLCSessionSettings* WslcSessionSettings, WSLCSessionFlags Flags, IWSLCSession** WslcSession);
    void ListSessions(_Out_ WSLCSessionInformation** Sessions, _Out_ ULONG* SessionsCount);
    void OpenSession(_In_ ULONG Id, _Out_ IWSLCSession** Session);
    void OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session);

private:
    // Iterates over all sessions, cleaning up released sessions.
    // The routine receives a SessionEntry& and can return an optional<T> to stop iteration.
    template <typename T>
    inline auto ForEachSession(const auto& Routine)
    {
        std::lock_guard lock(m_wslcSessionsLock);

        // Enforce noexcept: remove_if leaves the container in an unspecified
        // (partially-moved) state if the predicate throws. Callers must handle
        // errors via return values, not exceptions.
        static_assert(
            std::is_nothrow_invocable_v<decltype(Routine), SessionEntry&, wil::com_ptr<IWSLCSession>&>,
            "ForEachSession routine must be noexcept to preserve container invariants during remove_if");

        using TResult = std::conditional_t<std::is_same_v<T, void>, nullptr_t, std::optional<T>>;
        TResult result{};

        auto each = [&](SessionEntry& entry) {
            // Try to open the session via the service ref.
            // Fails with ERROR_OBJECT_NO_LONGER_EXISTS if released,
            // ERROR_INVALID_STATE if terminated, or RPC error if per-user process is dead.
            wil::com_ptr<IWSLCSession> lockedSession;
            if (FAILED_LOG(entry.Ref->OpenSession(&lockedSession)))
            {
                // Session is gone, drop the persistent reference if any.
                auto remove =
                    std::ranges::remove_if(m_persistentSessions, [&](const auto& e) { return e.first == entry.SessionId; });
                m_persistentSessions.erase(remove.begin(), remove.end());
                return true; // Remove from tracking
            }

            if constexpr (std::is_same_v<T, void>)
            {
                Routine(entry, lockedSession);
            }
            else
            {
                if (!result.has_value())
                {
                    result = Routine(entry, lockedSession);
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

    void AddSessionProcessToJobObject(_In_ IWSLCSessionFactory* Factory);
    WSLCSessionInitSettings CreateSessionSettings(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ const WSLCSessionSettings* Settings);
    void EnsureJobObjectCreated();
    static CallingProcessTokenInfo GetCallingProcessTokenInfo();
    static HRESULT CheckTokenAccess(const SessionEntry& Entry, const CallingProcessTokenInfo& TokenInfo);

    std::atomic<ULONG> m_nextSessionId{1};
    std::recursive_mutex m_wslcSessionsLock;

    // Job object that automatically terminates all child COM server processes
    // when this service exits or crashes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
    std::once_flag m_jobObjectInitFlag;
    wil::unique_handle m_sessionJobObject;

    // All sessions tracked via SessionEntry (which holds weak refs and service-side security info).
    // Sessions are automatically cleaned up when the underlying session is released.
    std::vector<SessionEntry> m_sessions;

    // Strong references to persistent sessions to keep them alive.
    // Session ID is stored alongside so cleanup doesn't require cross-process COM calls.
    std::vector<std::pair<ULONG, wil::com_ptr<IWSLCSession>>> m_persistentSessions;
};
} // namespace wsl::windows::service::wslc

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLCSessionManager
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCSessionManager, IFastRundown>,
      public wsl::windows::service::wslc::COMImplClass<wsl::windows::service::wslc::WSLCSessionManagerImpl>
{
public:
    NON_COPYABLE(WSLCSessionManager);
    NON_MOVABLE(WSLCSessionManager);

    WSLCSessionManager(wsl::windows::service::wslc::WSLCSessionManagerImpl* Impl);

    IFACEMETHOD(GetVersion)(_Out_ WSLCVersion* Version) override;
    IFACEMETHOD(CreateSession)(const WSLCSessionSettings* WslcSessionSettings, WSLCSessionFlags Flags, IWSLCSession** WslcSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLCSessionInformation** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLCSession** Session) override;
    IFACEMETHOD(OpenSessionByName)(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session) override;
};
