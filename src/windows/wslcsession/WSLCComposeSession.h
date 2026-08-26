/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCComposeSession.h

Abstract:

    Contains the minimal compose session implementation.

--*/

#pragma once

#include "ComposeSpec.h"
#include "wslc.h"
#include <mutex>
#include <vector>

namespace wsl::windows::service::wslc {

class WSLCSession;

class DECLSPEC_UUID("A8AA75A3-5B41-45AE-A847-1DF6CF35EAA0") WSLCComposeSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCComposeSession, IFastRundown, ISupportErrorInfo>
{
public:
    HRESULT RuntimeClassInitialize(WSLCSession* Session, std::wstring ConfigPath, ComposeSpec Spec, std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Containers);

    IFACEMETHOD(GetConfigPath)(_Out_ LPWSTR* Path) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLCContainerEntry** Containers, _Out_ ULONG* Count) override;
    IFACEMETHOD(Start()) override;
    IFACEMETHOD(Attach()) override;
    IFACEMETHOD(Stop)(_In_ ULONG Timeout) override;

    IFACEMETHOD(InterfaceSupportsErrorInfo)(_In_ REFIID InterfaceId) override;

private:
    std::mutex m_lock;
    WSLCSession* m_session{};
    std::wstring m_configPath;
    ComposeSpec m_spec;
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> m_containers;
};

} // namespace wsl::windows::service::wslc
