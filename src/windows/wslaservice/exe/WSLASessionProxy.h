/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionProxy.h

Abstract:

    Lightweight session proxy for lifetime and metadata management in the SYSTEM service.

    This proxy:
    - Implements IWeakReferenceSource for local weak reference support
    - Provides GetSession() for clients to obtain a direct reference to the remote session
    - Enables the session manager to track session lifetime without holding strong refs
    - Stores session metadata (ID, creator PID, display name, token info) for the session manager

    Clients should call GetSession() to get a direct COM reference to the per-user
    session process, bypassing the SYSTEM service for all operational calls.

--*/

#pragma once
#include "wslaservice.h"
#include <wrl/implements.h>
#include <wil/token_helpers.h>
#include <string>
#include <atomic>

namespace wsl::windows::service::wsla {

class WSLASessionManagerImpl;

struct CallingProcessTokenInfo
{
    wil::unique_tokeninfo_ptr<TOKEN_USER> Info;
    bool Elevated;
};

class WSLASessionProxy
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLASessionProxy, IFastRundown, Microsoft::WRL::FtmBase>
{
public:
    NON_COPYABLE(WSLASessionProxy);
    NON_MOVABLE(WSLASessionProxy);

    WSLASessionProxy(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ std::wstring DisplayName, _In_ CallingProcessTokenInfo TokenInfo, _In_ wil::com_ptr<IWSLASession> Session);

    ~WSLASessionProxy();

    // Accessors for session metadata
    ULONG GetId() const
    {
        return m_sessionId;
    }

    DWORD GetCreatorPid() const
    {
        return m_creatorPid;
    }

    const std::wstring& DisplayName() const
    {
        return m_displayName;
    }

    void CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR buffer, size_t bufferLength) const;

    PSID GetSid() const
    {
        return m_tokenInfo.Info->User.Sid;
    }

    bool IsTokenElevated() const
    {
        return m_tokenInfo.Elevated;
    }

    bool IsTerminated() const
    {
        return m_terminated;
    }

    // IWSLASessionProxy
    IFACEMETHOD(GetSession)(_Out_ IWSLASession** Session) override;
    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(Terminate)() override;

private:
    const ULONG m_sessionId;
    const DWORD m_creatorPid;
    const std::wstring m_displayName;
    const CallingProcessTokenInfo m_tokenInfo;
    std::atomic<bool> m_terminated{false};

    wil::com_ptr<IWSLASession> m_session;
};

} // namespace wsl::windows::service::wsla
