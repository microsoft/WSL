/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWUserSession.h

Abstract:

    TODO

--*/
#pragma once
#include "LSWVirtualMachine.h"

namespace wsl::windows::service::lsw {

class LSWUserSessionImpl
{
public:
    LSWUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo);
    LSWUserSessionImpl(LSWUserSessionImpl&&) = default;
    LSWUserSessionImpl& operator=(LSWUserSessionImpl&&) = default;

    ~LSWUserSessionImpl();

    PSID GetUserSid() const;

    HRESULT CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine);

    void OnVmTerminated(LSWVirtualMachine* machine);

private:
    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;

    std::recursive_mutex m_virtualMachinesLock;
    std::vector<LSWVirtualMachine*> m_virtualMachines;
};

class DECLSPEC_UUID("a9b7a1b9-0671-405c-95f1-e0612cb4ce8f") LSWUserSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ILSWUserSession, IFastRundown>
{
public:
    LSWUserSession(std::weak_ptr<LSWUserSessionImpl>&& Session);
    LSWUserSession(const LSWUserSession&) = delete;
    LSWUserSession& operator=(const LSWUserSession&) = delete;

    IFACEMETHOD(GetVersion)(_Out_ WSL_VERSION* Version) override;
    IFACEMETHOD(CreateVirtualMachine)(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine) override;

private:
    std::weak_ptr<LSWUserSessionImpl> m_session;
};

} // namespace wsl::windows::service::lsw
