/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManager.h

Abstract:

    Definition for WSLASessionManager.

    Session Lifetime Management:
    ----------------------------
    Sessions can be created as persistent or non-persistent:

    - Non-persistent sessions: Lifetime is tied to COM references. When all
      clients release their IWSLASession references, the session is terminated.
      Implemented via weak references to WSLASessionProxy objects.

    - Persistent sessions: Outlive their creating process. The session manager
      holds a strong reference to keep the session alive until explicitly
      terminated or the service shuts down.

    The WSLASessionProxy class wraps the remote IWSLASession (in the per-user
    process) and enables weak reference tracking in the SYSTEM service.

--*/

#pragma once
#include "wslaservice.h"
#include "WSLASessionProxy.h"
#include "COMImplClass.h"
#include "wslutil.h"
#include <atomic>
#include <algorithm>
#include <vector>
#include <mutex>
#include <string>

namespace wslutil = wsl::windows::common::wslutil;

namespace wsl::windows::service::wsla {

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
    // Iterates over all sessions, cleaning up released non-persistent sessions.
    // The routine receives a WSLASessionProxy& and can return an optional<T> to stop iteration.
    template <typename T>
    inline auto ForEachSession(const auto& Routine)
    {
        std::lock_guard lock(m_wslaSessionsLock);

        using TResult = std::conditional_t<std::is_same_v<T, void>, nullptr_t, std::optional<T>>;
        TResult result{};

        auto each = [&](const Microsoft::WRL::ComPtr<IWeakReference>& WeakRef) {
            // Try to resolve the weak reference
            Microsoft::WRL::ComPtr<IWSLASession> lockedSession;
            THROW_IF_FAILED(WeakRef->Resolve(__uuidof(IWSLASession), reinterpret_cast<IInspectable**>(lockedSession.GetAddressOf())));

            if (!lockedSession)
            {
                return true; // Session proxy was released, remove from tracking
            }

            // Get the proxy (we know it's a WSLASessionProxy because we created it)
            auto* proxy = static_cast<WSLASessionProxy*>(lockedSession.Get());

            // If the session was terminated, drop persistent reference so it can be cleaned up
            if (proxy->IsTerminated())
            {
                auto remove = std::ranges::remove_if(m_persistentSessions, [&](const auto& e) { return e.Get() == proxy; });
                m_persistentSessions.erase(remove.begin(), remove.end());
                return true; // Remove from tracking
            }

            if constexpr (std::is_same_v<T, void>)
            {
                Routine(*proxy);
            }
            else
            {
                if (!result.has_value())
                {
                    result = Routine(*proxy);
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

    void AddSessionProcessToJobObject(_In_ IWSLASession* Session);
    WSLA_SESSION_INIT_SETTINGS CreateSessionSettings(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ const WSLA_SESSION_SETTINGS* Settings);
    void EnsureJobObjectCreated();
    static CallingProcessTokenInfo GetCallingProcessTokenInfo();
    static HRESULT CheckTokenAccess(const WSLASessionProxy& Session, const CallingProcessTokenInfo& TokenInfo);

    std::atomic<ULONG> m_nextSessionId{1};
    std::recursive_mutex m_wslaSessionsLock;

    // Job object that automatically terminates all child COM server processes
    // when this service exits or crashes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
    std::once_flag m_jobObjectInitFlag;
    wil::unique_handle m_sessionJobObject;

    // All sessions tracked via weak references. Non-persistent sessions are
    // automatically cleaned up when all client references are released.
    std::vector<Microsoft::WRL::ComPtr<IWeakReference>> m_sessions;

    // Strong references to persistent sessions to keep them alive.
    std::vector<Microsoft::WRL::ComPtr<WSLASessionProxy>> m_persistentSessions;
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
