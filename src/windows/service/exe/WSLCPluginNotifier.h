// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "wslc.h"
#include "PluginManager.h"
#include <wil/resource.h>
#include <vector>

namespace wsl::windows::service::wslc {

//
// WSLCPluginNotifier - SYSTEM service implementation of IWSLCPluginNotifier.
// Lives in the SYSTEM service and is passed (via COM marshalling) as a top-level
// parameter to the per-user WSLC session process via IWSLCSessionFactory::CreateSession.
// The per-user process invokes the On* methods, which dispatch to PluginManager.
//
class DECLSPEC_UUID("E29B0F1A-4E18-4F09-83A2-2D6B1B9F8C4D") WSLCPluginNotifier
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCPluginNotifier, IFastRundown>
{
public:
    NON_COPYABLE(WSLCPluginNotifier);
    NON_MOVABLE(WSLCPluginNotifier);

    WSLCPluginNotifier(
        wsl::windows::service::PluginManager& Plugins,
        ULONG SessionId,
        DWORD CreatorPid,
        std::wstring DisplayName,
        wil::shared_handle UserToken,
        std::vector<BYTE>&& UserSid);

    IFACEMETHOD(OnContainerStarted)(_In_ LPCSTR InspectJson) override;
    IFACEMETHOD(OnContainerStopping)(_In_ LPCSTR ContainerId) override;
    IFACEMETHOD(OnImageCreated)(_In_ LPCSTR InspectJson) override;
    IFACEMETHOD(OnImageDeleted)(_In_ LPCSTR ImageId) override;

private:
    wsl::windows::service::PluginManager& m_plugins;
    std::wstring m_displayName;
    wil::shared_handle m_userToken;
    std::vector<BYTE> m_userSid;
    WSLCSessionInformation m_sessionInfo{};
};

} // namespace wsl::windows::service::wslc
