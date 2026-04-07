/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCLocalRegistry.h

Abstract:

    Helper class that starts a local Docker registry:3 container inside a WSLC
    session, optionally configured with htpasswd basic authentication.  Intended
    for use in both unit tests and E2E tests that need a private registry without
    an external dependency.

--*/

#pragma once
#include "WSLCContainerLauncher.h"

namespace wsl::windows::common {

class WSLCLocalRegistry
{
public:
    NON_COPYABLE(WSLCLocalRegistry);
    DEFAULT_MOVABLE(WSLCLocalRegistry);
    ~WSLCLocalRegistry();

    static WSLCLocalRegistry Start(IWSLCSession& Session, const std::string& Username = {}, const std::string& Password = {});

    const char* GetServerAddress() const;
    const std::string& GetUsername() const;
    const std::string& GetPassword() const;

private:
    WSLCLocalRegistry(IWSLCSession& session, RunningWSLCContainer&& container, std::string&& username, std::string&& password);

    wil::com_ptr<IWSLCSession> m_session;
    std::string m_username;
    std::string m_password;
    RunningWSLCContainer m_container;
};

} // namespace wsl::windows::common
