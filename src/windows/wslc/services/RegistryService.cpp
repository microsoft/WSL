/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryService.cpp

Abstract:

    This file contains the RegistryService implementation

--*/

#include "RegistryService.h"
#include <wslutil.h>

using namespace wsl::windows::common::wslutil;

namespace {

std::string ResolveCredentialKey(const std::string& serverAddress)
{
    // Normalize known Docker Hub aliases to the canonical DefaultServer key,
    // matching Docker CLI's getAuthConfigKey() behavior.
    if (serverAddress == "docker.io" || serverAddress == "index.docker.io" || serverAddress == "registry-1.docker.io")
    {
        return wsl::windows::wslc::services::RegistryService::DefaultServer;
    }

    return serverAddress;
}
} // namespace

namespace wsl::windows::wslc::services {

void RegistryService::Store(const std::string& serverAddress, const std::string& credential)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());
    THROW_HR_IF(E_INVALIDARG, credential.empty());

    auto storage = OpenCredentialStorage();
    storage->Store(ResolveCredentialKey(serverAddress), credential);
}

std::optional<std::string> RegistryService::Get(const std::string& serverAddress)
{
    if (serverAddress.empty())
    {
        return std::nullopt;
    }

    auto storage = OpenCredentialStorage();
    return storage->Get(ResolveCredentialKey(serverAddress));
}

void RegistryService::Erase(const std::string& serverAddress)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());

    auto storage = OpenCredentialStorage();
    storage->Erase(ResolveCredentialKey(serverAddress));
}

std::vector<std::wstring> RegistryService::List()
{
    auto storage = OpenCredentialStorage();
    return storage->List();
}

std::string RegistryService::Authenticate(
    wsl::windows::wslc::models::Session& session, const std::string& serverAddress, const std::string& username, const std::string& password)
{
    wil::unique_cotaskmem_ansistring identityToken;
    THROW_IF_FAILED(session.Get()->Authenticate(serverAddress.c_str(), username.c_str(), password.c_str(), &identityToken));

    // If the registry returned an identity token, use it. Otherwise fall back to username/password.
    if (identityToken && strlen(identityToken.get()) > 0)
    {
        return BuildRegistryAuthHeader(identityToken.get(), serverAddress);
    }

    return BuildRegistryAuthHeader(username, password, serverAddress);
}

} // namespace wsl::windows::wslc::services
