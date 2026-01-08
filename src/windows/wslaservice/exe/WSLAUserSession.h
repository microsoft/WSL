/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.h

Abstract:

    TODO

--*/

#pragma once
#include "WSLAVirtualMachine.h"
#include "WSLASession.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_set>

namespace wsl::windows::service::wsla {

class WSLAUserSessionImpl
{
public:
    WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo);
    WSLAUserSessionImpl(WSLAUserSessionImpl&&) = default;
    WSLAUserSessionImpl& operator=(WSLAUserSessionImpl&&) = default;

    ~WSLAUserSessionImpl();

    PSID GetUserSid() const;

    HRESULT CreateSession(const WSLA_SESSION_SETTINGS* Settings, IWSLASession** WslaSession);
    HRESULT OpenSessionByName(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session);
    HRESULT ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount);

private:
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

            WSLASession* SessionImpl{};
            THROW_IF_FAILED(lockedSession->GetImpl(reinterpret_cast<void**>(&SessionImpl)));

            if constexpr (std::is_same_v<T, void>)
            {
                Routine(*SessionImpl);
            }
            else
            {
                result = Routine(*SessionImpl);
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

    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;

    std::atomic<ULONG> m_nextSessionId{1};
    std::recursive_mutex m_wslaSessionsLock;

    std::vector<Microsoft::WRL::ComPtr<IWeakReference>> m_sessions;
};

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") WSLAUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAUserSession, IFastRundown>
{
public:
    WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session);
    WSLAUserSession(const WSLAUserSession&) = delete;
    WSLAUserSession& operator=(const WSLAUserSession&) = delete;

    IFACEMETHOD(GetVersion)(_Out_ WSLA_VERSION* Version) override;
    IFACEMETHOD(CreateSession)(const WSLA_SESSION_SETTINGS* WslaSessionSettings, IWSLASession** WslaSession) override;
    IFACEMETHOD(ListSessions)(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount) override;
    IFACEMETHOD(OpenSession)(_In_ ULONG Id, _Out_ IWSLASession** Session) override;
    IFACEMETHOD(OpenSessionByName)(_In_ LPCWSTR DisplayName, _Out_ IWSLASession** Session) override;

private:
    std::weak_ptr<WSLAUserSessionImpl> m_session;
};

} // namespace wsl::windows::service::wsla
