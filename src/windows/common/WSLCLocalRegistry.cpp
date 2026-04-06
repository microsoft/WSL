/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCLocalRegistry.cpp

Abstract:

    Implementation of WSLCLocalRegistry.

--*/
#include "WSLCLocalRegistry.h"

using wsl::windows::common::RunningWSLCContainer;
using wsl::windows::common::WSLCContainerLauncher;
using wsl::windows::common::WSLCLocalRegistry;

namespace {

constexpr auto c_registryImage = "wslc-registry:latest";

std::vector<std::string> BuildRegistryEnv(const std::string& username, const std::string& password)
{
    std::vector<std::string> env = {
        "REGISTRY_HTTP_ADDR=0.0.0.0:5000",
    };

    if (!username.empty())
    {
        env.push_back(std::format("USERNAME={}", username));
        env.push_back(std::format("PASSWORD={}", password));
    }

    return env;
}

} // namespace

WSLCLocalRegistry::WSLCLocalRegistry(
    IWSLCSession& session,
    RunningWSLCContainer&& container,
    std::string&& username,
    std::string&& password) :
    m_session(wil::com_ptr<IWSLCSession>(&session)),
    m_username(std::move(username)),
    m_password(std::move(password)),
    m_container(std::move(container))
{
}

WSLCLocalRegistry::~WSLCLocalRegistry()
{
    // Delete the container first while the session is still active.
    m_container.Reset();
}

WSLCLocalRegistry WSLCLocalRegistry::Start(
    IWSLCSession& session, const std::string& username, const std::string& password)
{
    auto env = BuildRegistryEnv(username, password);

    WSLCContainerLauncher launcher(c_registryImage, {}, {}, env);
    launcher.SetEntrypoint({"/entrypoint.sh"});
    launcher.AddPort(5000, 5000, AF_INET);

    auto container = launcher.Launch(session, WSLCContainerStartFlagsNone);

    return WSLCLocalRegistry(
        session,
        std::move(container),
        std::string(username),
        std::string(password));
}

const char* WSLCLocalRegistry::GetServerAddress() const
{
    return "127.0.0.1:5000";
}

const std::string& WSLCLocalRegistry::GetUsername() const
{
    return m_username;
}

const std::string& WSLCLocalRegistry::GetPassword() const
{
    return m_password;
}
