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
    auto input = serverAddress;

    // Strip scheme
    if (auto pos = input.find("://"); pos != std::string::npos)
    {
        input = input.substr(pos + 3);
    }

    // Strip path
    if (auto pos = input.find('/'); pos != std::string::npos)
    {
        input = input.substr(0, pos);
    }

    // Map Docker Hub aliases to canonical key.
    if (input == "docker.io" || input == "index.docker.io")
    {
        return wsl::windows::wslc::services::RegistryService::DefaultServer;
    }

    return input;
}
} // namespace

namespace wsl::windows::wslc::services {

// Sentinel username matching Docker's convention for identity-token credentials.
static constexpr auto TokenUsername = "<token>";

void RegistryService::Store(const std::string& serverAddress, const std::string& username, const std::string& secret)
{
    THROW_HR_IF(E_INVALIDARG, serverAddress.empty());
    THROW_HR_IF(E_INVALIDARG, secret.empty());

    auto storage = OpenCredentialStorage();
    storage->Store(ResolveCredentialKey(serverAddress), username, secret);
}

std::string RegistryService::Get(const std::string& serverAddress)
{
    auto storage = OpenCredentialStorage();
    auto key = ResolveCredentialKey(serverAddress);
    auto [username, secret] = storage->Get(key);

    if (username == TokenUsername)
    {
        return BuildRegistryAuthHeader(secret);
    }

    return BuildRegistryAuthHeader(username, secret);
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

std::pair<std::string, std::string> RegistryService::Authenticate(
    wsl::windows::wslc::models::Session& session, const std::string& serverAddress, const std::string& username, const std::string& password)
{
    wil::unique_cotaskmem_ansistring identityToken;
    THROW_IF_FAILED(session.Get()->Authenticate(serverAddress.c_str(), username.c_str(), password.c_str(), &identityToken));

    // If the registry returned an identity token, use it. Otherwise fall back to username/password.
    if (identityToken && strlen(identityToken.get()) > 0)
    {
        return {TokenUsername, identityToken.get()};
    }

    return {username, password};
}

} // namespace wsl::windows::wslc::services
