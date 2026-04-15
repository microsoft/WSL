/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryService.h

Abstract:

    This file contains the RegistryService definition

--*/
#pragma once

#include "ICredentialStorage.h"
#include "SessionModel.h"

namespace wsl::windows::wslc::services {

// High-level registry authentication service.
// Delegates credential persistence to ICredentialStorage (selected via OpenCredentialStorage).
class RegistryService
{
public:
    static void Store(const std::string& serverAddress, const std::string& credential);
    static std::optional<std::string> Get(const std::string& serverAddress);
    static void Erase(const std::string& serverAddress);
    static std::vector<std::wstring> List();

    // Authenticates with a registry via the session's Docker engine.
    // Returns a base64-encoded auth header ready to store and pass to push/pull.
    static std::string Authenticate(
        wsl::windows::wslc::models::Session& session, const std::string& serverAddress, const std::string& username, const std::string& password);

    // Default registry server address used when no explicit server is provided.
    static constexpr auto DefaultServer = "https://index.docker.io/v1/";
};

} // namespace wsl::windows::wslc::services
