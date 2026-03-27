/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionFactory.h

Abstract:

    IWSLCSessionFactory implementation.

    This factory runs in the per-user COM server process and is created by
    the SYSTEM service via CoCreateInstanceAsUser. It creates WSLCSession
    objects and their corresponding IWSLCSessionReference weak references.

    The factory is responsible for:
    - Creating the WSLCSession in the per-user security context
    - Creating the IWSLCSessionReference that holds a weak reference
    - Providing the process handle for job object management

--*/

#pragma once
#include "wslc.h"
#include <wrl/implements.h>
#include <wil/com.h>
#include <functional>

namespace wsl::windows::service::wslc {

class DECLSPEC_UUID("9FCD2067-9FC6-4EFA-9EB0-698169EBF7D3") WSLCSessionFactory
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCSessionFactory, IFastRundown>
{
public:
    NON_COPYABLE(WSLCSessionFactory);
    NON_MOVABLE(WSLCSessionFactory);

    WSLCSessionFactory() = default;

    // Sets a callback invoked when the session in this process is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()>&& callback);

    // IWSLCSessionFactory
    IFACEMETHOD(CreateSession)
    (_In_ const WSLCSessionInitSettings* Settings, _In_ IWSLCVirtualMachine* Vm, _Out_ IWSLCSession** Session, _Out_ IWSLCSessionReference** ServiceRef)
        override;

    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;

private:
    std::function<void()> m_destructionCallback;
};

} // namespace wsl::windows::service::wslc
