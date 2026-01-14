/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManager.h

Abstract:

    Definition for WSLASessionManager.

--*/

#pragma once
#include "WSLAVirtualMachine.h"
#include "WSLASession.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_set>

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLASessionManager
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASessionManager, IFastRundown>
{
public:
    NON_COPYABLE(WSLASessionManager);
    NON_MOVEABLE(WSLASessionManager);

    WSLASessionManager();

    ~WSLASessionManager();

    IFACEMETHOD(GetVersion)(_Out_ WSLA_VERSION* Version) override;
    IFACEMETHOD(CreateSession)(const WSLA_SESSION_SETTINGS* WslaSessionSettings, WSLASessionFlags Flags, IWSLASession** WslaSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLASession** Session) override;
    IFACEMETHOD(OpenSessionByName)(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session) override;

private:
    struct CallingProcessTokenInfo
    {
        wil::unique_tokeninfo_ptr<TOKEN_USER> TokenUser;
        bool Elevated;
    };

    
    template <typename T>
    inline auto ForEachSession(const auto& Routine)
    {
        std::lock_guard lock(m_wslaSessionsLock);

        using TResult = std::conditional_t<std::is_same_v<T, void>, nullptr_t, std::optional<T>>;
        TResult result{};
        auto each = [&](const Microsoft::WRL::ComPtr<IWeakReference>& Session) {
            Microsoft::WRL::ComPtr<IWSLASessionImpl> lockedSession;
            THROW_IF_FAILED(Session->Resolve(lockedSession.GetAddressOf()));
            if (!lockedSession)
            {
                return true; // Object is released, remove from the session list.
            }

            // N.B. lockedSession has a reference to the COM object.
            WSLASession* SessionImpl{};
            THROW_IF_FAILED(lockedSession->GetImplNoRef(&SessionImpl));

            // If the session is terminated, drop its reference so it can be deleted (in case of persistent sessions)
            if (SessionImpl->Terminated())
            {
                auto remove =
                    std::ranges::remove_if(m_persistentSessions, [&](const auto& e) { return SessionImpl->GetId() == e->GetId(); });

                WI_ASSERT(remove.end() - remove.begin() <= 1);

                m_persistentSessions.erase(remove.begin(), remove.end());
                return true;
            }

            if constexpr (std::is_same_v<T, void>)
            {
                Routine(*SessionImpl);
            }
            else
            {
                if (!result.has_value())
                {
                    result = Routine(*SessionImpl);
                }
            }

            return false;
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

    static CallingProcessTokenInfo GetCallingProcessTokenInfo();
    static HRESULT CheckTokenAccess(const WSLASession& Session, const CallingProcessTokenInfo& TokenInfo);

    std::atomic<ULONG> m_nextSessionId{1};
    std::shared_mutex m_wslaSessionsLock;

    // Persistent sessions that outlive their creating process.
    std::vector<Microsoft::WRL::ComPtr<WSLASession>> m_persistentSessions;
    std::vector<Microsoft::WRL::ComPtr<IWeakReference>> m_sessions;
};

} // namespace wsl::windows::service::wsla
