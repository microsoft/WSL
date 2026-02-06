/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionFactory.h

Abstract:

    IWSLASessionFactory implementation.

    This factory runs in the per-user COM server process and is created by
    the SYSTEM service via CoCreateInstanceAsUser. It creates WSLASession
    objects and their corresponding IWSLASessionReference weak references.

    The factory is responsible for:
    - Creating the WSLASession in the per-user security context
    - Creating the IWSLASessionReference that holds a weak reference
    - Providing the process handle for job object management

--*/

#pragma once
#include "wslaservice.h"
#include <wrl/implements.h>
#include <wil/com.h>
#include <functional>

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("C4E8F291-3B5D-4A7C-9E12-8F6A4D2B7C91") WSLASessionFactory
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASessionFactory, IFastRundown>
{
public:
    NON_COPYABLE(WSLASessionFactory);
    NON_MOVABLE(WSLASessionFactory);

    WSLASessionFactory() = default;

    // Sets a callback invoked when the session in this process is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()> callback);

    // IWSLASessionFactory
    IFACEMETHOD(CreateSession)
    (_In_ const WSLA_SESSION_INIT_SETTINGS* Settings, _In_ IWSLAVirtualMachine* Vm, _Out_ IWSLASession** Session, _Out_ IWSLASessionReference** ServiceRef)
        override;

    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;

private:
    std::function<void()> m_destructionCallback;
};

} // namespace wsl::windows::service::wsla
