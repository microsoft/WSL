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

void PopulateHtpasswd(IWSLCSession& session, const std::filesystem::path& storagePath, const std::string& username, const std::string& password)
{
    LOG_IF_FAILED(session.PullImage(c_htpasswdImage, nullptr, nullptr));

    // Write the htpasswd file into /data/ on the shared folder.
    const auto command = std::format("htpasswd -Bbn '{}' '{}' > /data/htpasswd", username, password);

    WSLCContainerLauncher launcher("httpd:2", {}, {"/bin/sh", "-c", command});
    launcher.AddVolume(storagePath.wstring(), "/data", false);
    launcher.SetContainerFlags(WSLCContainerFlagsRm);

    auto container = launcher.Launch(session);
    auto result = container.GetInitProcess().WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());
}

std::vector<std::string> BuildRegistryEnv(bool withAuth)
{
    // Builds the environment variable list for the registry:3 container.
    std::vector<std::string> env = {
        "REGISTRY_HTTP_ADDR=0.0.0.0:5000",
        "REGISTRY_STORAGE_FILESYSTEM_ROOTDIRECTORY=/data/registry",
        "REGISTRY_PROXY_REMOTEURL=https://registry-1.docker.io"
    };

    if (withAuth)
    {
        env.push_back("REGISTRY_AUTH=htpasswd");
        env.push_back("REGISTRY_AUTH_HTPASSWD_REALM=WSLC Test Registry");
        env.push_back("REGISTRY_AUTH_HTPASSWD_PATH=/data/htpasswd");
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
    IWSLCSession& session, const std::filesystem::path& storagePath, const std::string& username, const std::string& password)
{
    // Ensure required images are available.
    LOG_IF_FAILED(session.PullImage(c_registryImage, nullptr, nullptr));

    // If credentials are provided, populate the htpasswd file on the folder.
    if (!username.empty())
    {
        PopulateHtpasswd(session, storagePath, username, password);
    }

    // Launch the registry container.
    WSLCContainerLauncher launcher(
        c_registryImage,
        {},
        {},
        BuildRegistryEnv(!username.empty()));

    launcher.AddPort(5000, 5000, AF_INET);
    launcher.AddVolume(storagePath.wstring(), "/data", false);

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
