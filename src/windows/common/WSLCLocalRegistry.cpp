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

constexpr auto c_registryImage = "registry:3";
constexpr auto c_htpasswdImage = "httpd:2";

std::string GenerateHtpasswd(IWSLCSession& session, const std::string& username, const std::string& password)
{
    THROW_IF_FAILED(session.PullImage(c_htpasswdImage, nullptr, nullptr));

    const auto command = std::format("htpasswd -Bbn '{}' '{}'", username, password);

    WSLCContainerLauncher launcher(c_htpasswdImage, {}, {"/bin/sh", "-c", command});
    launcher.SetContainerFlags(WSLCContainerFlagsRm);

    auto container = launcher.Launch(session);
    auto result = container.GetInitProcess().WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());

    auto output = result.Output[1];
    output.erase(output.find_last_not_of("\n\r") + 1);

    THROW_HR_IF_MSG(E_FAIL, output.empty(), "%hs", launcher.FormatResult(result).c_str());
    return output;
}

std::vector<std::string> BuildRegistryEnv(IWSLCSession& session, const std::string& username, const std::string& password)
{
    std::vector<std::string> env = {
        "REGISTRY_HTTP_ADDR=0.0.0.0:5000",
    };

    if (!username.empty())
    {
        auto htpasswdEntry = GenerateHtpasswd(session, username, password);

        env.push_back(std::format("HTPASSWD_CONTENT={}", htpasswdEntry));
        env.push_back("REGISTRY_AUTH=htpasswd");
        env.push_back("REGISTRY_AUTH_HTPASSWD_REALM=WSLC Test Registry");
        env.push_back("REGISTRY_AUTH_HTPASSWD_PATH=/htpasswd");
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
    THROW_IF_FAILED(session.PullImage(c_registryImage, nullptr, nullptr));    

    auto env = BuildRegistryEnv(session, username, password);

    WSLCContainerLauncher launcher(c_registryImage, {}, {}, env);
    launcher.AddPort(5000, 5000, AF_INET);

    if (!username.empty())
    {
        launcher.SetEntrypoint({"/bin/sh", "-c", "echo \"$HTPASSWD_CONTENT\" > /htpasswd && registry serve /etc/distribution/config.yml"});
    }
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
