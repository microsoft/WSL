/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASession, IFastRundown>

{
public:
    //WSLASession(std::weak_ptr<WSLASessionImpl>&& Session);
    WSLASession(const WSLA_SESSION_CONFIGURATION& SessionConfiguration);
    //WSLASession(const WSLASession&) = delete;
   // WSLASession& operator=(const WSLASession&) = delete;

    /* void Start();
    void OnUserSessionTerminating();

    IFACEMETHOD(WSLAStartContainer)(_In_ WSLA_CONTAINER_OPTIONS* ContainerOptions, _Out_ IWSLAContainer** Container) override;
    IFACEMETHOD(WSLAStopContainer)(_In_ LPCSTR ContainerId) override;
    IFACEMETHOD(WSLARestartContainer)(_In_ WSLA_CONTAINER_OPTIONS* ContainerOptions, _In_ LPCSTR ContainerId, _Out_ IWSLAContainer** Container) override;
    IFACEMETHOD(WSLAGetContainerState)(_In_ LPCSTR ContainerId, _Out_ IWSLAContainer** Container) override;
    IFACEMETHOD(WSLAListContainers)(_In_ WSLA_CONTAINER_OPTIONS* ContainerOptions, _In_ LPCSTR ContainerId, _Out_ ContainerState& State) override; */
    // TODO: add more interface methods here

    
    IFACEMETHOD(GetDisplayName)(LPWSTR* DisplayName);

private:
    WSLA_SESSION_CONFIGURATION m_sessionConfig;

    //WSLAUserSessionImpl* m_userSession;
    //std::weak_ptr<WSLASessionImpl> m_wslaSession;
};
} // namespace wsl::windows::service::wsla
