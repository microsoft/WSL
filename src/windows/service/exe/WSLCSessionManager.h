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

    Microsoft::WRL::ComPtr<IWSLCPluginNotifier> PluginNotifier;

    // Whether OnSessionStopping has been fired already; ensures it is fired exactly once.
    bool StoppingNotified = false;

    wil::shared_handle UserToken;
    std::vector<BYTE> UserSid;

    wil::unique_handle JobObject;
};

class WSLCSessionManagerImpl
{
public:
    NON_COPYABLE(WSLCSessionManagerImpl);
    NON_MOVABLE(WSLCSessionManagerImpl);

    WSLCSessionManagerImpl();
    ~WSLCSessionManagerImpl();

    void CreateSession(
        _In_ const WSLCSessionSettings* WslcSessionSettings,
        _In_ WSLCSessionFlags Flags,
        _In_opt_ IWarningCallback* WarningCallback,
        _Out_ IWSLCSession** WslcSession);
    void EnterSession(_In_ LPCWSTR DisplayName, _In_ LPCWSTR StoragePath, _In_opt_ IWarningCallback* WarningCallback, _Out_ IWSLCSession** WslcSession);
    void ListSessions(_Out_ WSLCSessionListEntry** Sessions, _Out_ ULONG* SessionsCount);
    void OpenSession(_In_ ULONG Id, _Out_ IWSLCSession** Session);
    void OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session);

    static WSLCSessionManagerImpl* Instance() noexcept;

private:
    // Resolves the default session name for a caller: appends the username
    // from the token SID so different users don't collide.
    static std::wstring ResolveDefaultSessionName(const CallingProcessTokenInfo& TokenInfo);

    // Returns true if the name matches a reserved default session prefix.
    static bool IsReservedSessionName(LPCWSTR Name);

    // Iterates over all sessions, cleaning up released sessions. The CALLER
    // MUST hold m_wslcSessionsLock. Sessions whose backing process is gone are
    // moved out of tracking into `DeadSessions` so the caller can dispatch
    // their OnWslcSessionStopping plugin notification (via DispatchSessionStopping)
    // AFTER releasing the lock — the notification is an out-of-process call and
    // must not run under the lock. The routine receives a SessionEntry& and can
    // return an optional<T> to stop iteration.
    template <typename T>
    inline auto ForEachSessionLockHeld(const auto& Routine, std::vector<SessionEntry>& DeadSessions)
    {
        // Enforce noexcept: remove_if leaves the container in an unspecified
        // (partially-moved) state if the predicate throws. Callers must handle
        // errors via return values, not exceptions.
        static_assert(
            std::is_nothrow_invocable_v<decltype(Routine), SessionEntry&, wil::com_ptr<IWSLCSession>&>,
            "ForEachSession routine must be noexcept to preserve container invariants during remove_if");

        using TResult = std::conditional_t<std::is_same_v<T, void>, std::nullptr_t, std::optional<T>>;
        TResult result{};

        auto each = [&](SessionEntry& entry) {
            // Try to open the session via the service ref.
            // Fails with ERROR_OBJECT_NO_LONGER_EXISTS if released,
            // ERROR_INVALID_STATE if terminated, or RPC error if per-user process is dead.
            wil::com_ptr<IWSLCSession> lockedSession;
            if (FAILED_LOG(entry.Ref->OpenSession(&lockedSession)))
            {
                // Session is gone: move it out for deferred OnWslcSessionStopping
                // dispatch (must happen outside the lock) and drop any persistent
                // reference. The StoppingNotified flag is intentionally left
                // untouched here; DispatchSessionStopping flips it so the
                // notification fires exactly once.
                // Reserve the slot up front so push_back cannot reallocate (and
                // therefore cannot throw) after `entry` is moved-from. Combined
                // with SessionEntry's noexcept move, this guarantees `entry` is
                // only consumed once the destination slot is known to exist.
                DeadSessions.reserve(DeadSessions.size() + 1);
                try
                {
                    DeadSessions.push_back(std::move(entry));
                }
                catch (...)
                {
                    // Defensive: if queuing for the deferred stopping dispatch
                    // still fails, keep the session tracked and reap it on a
                    // later pass rather than dropping it without unregistering.
                    LOG_CAUGHT_EXCEPTION();
                    return false;
                }

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

    [[nodiscard]] wil::unique_handle CreateSessionProcessJob(_In_ IWSLCSessionFactory* Factory);

    // Convenience wrapper that takes m_wslcSessionsLock internally and dispatches
    // OnWslcSessionStopping for any dead sessions after releasing the lock. Use
    // this from call sites that do NOT already hold the lock.
    template <typename T>
    inline auto ForEachSession(const auto& Routine)
    {
        std::vector<SessionEntry> deadSessions;

        using TResult = std::conditional_t<std::is_same_v<T, void>, std::nullptr_t, std::optional<T>>;
        TResult result{};

        {
            std::lock_guard lock(m_wslcSessionsLock);
            if constexpr (std::is_same_v<T, void>)
            {
                ForEachSessionLockHeld<T>(Routine, deadSessions);
            }
            else
            {
                result = ForEachSessionLockHeld<T>(Routine, deadSessions);
            }
        }

        // Safe to invoke plugin callbacks now that the lock is released.
        for (auto& entry : deadSessions)
        {
            DispatchSessionStopping(entry);
        }

        if constexpr (std::is_same_v<T, void>)
        {
            return;
        }
        else
        {
            return result;
        }
    }

    WSLCSessionInitSettings CreateSessionSettings(
        _In_ ULONG SessionId, _In_ LPCWSTR CreatorProcessName, _In_ const WSLCSessionSettings* Settings, _In_ LPCWSTR ResolvedDisplayName);
    static CallingProcessTokenInfo GetCallingProcessTokenInfo();
    static HRESULT CheckTokenAccess(const SessionEntry& Entry, const CallingProcessTokenInfo& TokenInfo);

    // Fires OnWslcSessionStopping for `entry` exactly once (guarded by
    // entry.StoppingNotified) and then unregisters the session from the plugin
    // session reference map. MUST be called WITHOUT m_wslcSessionsLock held —
    // the plugin notification is dispatched out-of-process to wslpluginhost.exe.
    void DispatchSessionStopping(SessionEntry& entry) noexcept;

    std::atomic<ULONG> m_nextSessionId{1};
    std::mutex m_wslcSessionsLock;

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
    IFACEMETHOD(IsClientVersionSupported)(_In_ const WSLCVersion* ClientVersion, _Out_ BOOL* IsSupported) override;
    IFACEMETHOD(CreateSession)(
        const WSLCSessionSettings* WslcSessionSettings, WSLCSessionFlags Flags, IWarningCallback* WarningCallback, IWSLCSession** WslcSession) override;
    IFACEMETHOD(EnterSession)(_In_ LPCWSTR DisplayName, _In_ LPCWSTR StoragePath, IWarningCallback* WarningCallback, IWSLCSession** WslcSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLCSessionListEntry** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLCSession** Session) override;
    IFACEMETHOD(OpenSessionByName)(_In_ LPCWSTR DisplayName, _Out_ IWSLCSession** Session) override;
};
