/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionReference.h

Abstract:

    IWSLASessionReference implementation.

    This object lives in the per-user COM server process and holds a weak
    reference to the WSLASession. The SYSTEM service holds these references
    to track active sessions without preventing session cleanup when clients
    release their references.

    When OpenSession() is called:
    - If the session is still alive, it returns S_OK with a strong reference
    - If the session has been released, it returns ERROR_OBJECT_NO_LONGER_EXISTS
    - If the session has been terminated, it returns ERROR_INVALID_STATE

--*/

#pragma once
#include "wslaservice.h"
#include <wrl/implements.h>
#include <wil/com.h>
#include <string>

namespace wsl::windows::service::wsla {

class WSLASession;

class WSLASessionReference
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASessionReference, IFastRundown, Microsoft::WRL::FtmBase>
{
public:
    NON_COPYABLE(WSLASessionReference);
    NON_MOVABLE(WSLASessionReference);

    WSLASessionReference(_In_ WSLASession* Session);

    ~WSLASessionReference();

    // IWSLASessionReference
    IFACEMETHOD(OpenSession)(_Out_ IWSLASession** Session) override;
    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(GetCreatorPid)(_Out_ DWORD* Pid) override;
    IFACEMETHOD(GetDisplayName)(_Out_ LPWSTR* DisplayName) override;
    IFACEMETHOD(GetSid)(_Out_ LPWSTR* Sid) override;
    IFACEMETHOD(IsElevated)(_Out_ BOOL* Elevated) override;
    IFACEMETHOD(Terminate)() override;

private:
    const ULONG m_sessionId;
    const DWORD m_creatorPid;
    const std::wstring m_displayName;
    const wil::unique_hlocal_string m_sidString;
    const bool m_elevated;

    Microsoft::WRL::ComPtr<IWeakReference> m_weakSession;
};

} // namespace wsl::windows::service::wsla
