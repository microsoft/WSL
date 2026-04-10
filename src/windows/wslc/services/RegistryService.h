/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryService.h

Abstract:

    This file contains the RegistryService definition

--*/
#pragma once

#include "SessionModel.h"
#include "UserSettings.h"

namespace wsl::windows::wslc::services {

using wsl::windows::wslc::settings::CredentialStoreType;

// Credential store that persists opaque credential strings keyed by server address.
// Supports Windows Credential Manager and DPAPI-encrypted JSON file backends.
class RegistryService
{
public:
    static void Store(CredentialStoreType backend, const std::string& serverAddress, const std::string& credential);
    static std::optional<std::string> Get(CredentialStoreType backend, const std::string& serverAddress);
    static void Erase(CredentialStoreType backend, const std::string& serverAddress);
    static std::vector<std::string> List(CredentialStoreType backend);

    // Authenticates with a registry via the session's Docker engine.
    // Returns a base64-encoded auth header ready to store and pass to push/pull.
    static std::string Authenticate(
        wsl::windows::wslc::models::Session& session, const std::string& serverAddress, const std::string& username, const std::string& password);

    // Default registry server address used when no explicit server is provided.
    static constexpr auto DefaultServer = "https://index.docker.io/v1/";

private:
    // WinCred helpers
    static void WinCredStoreCredential(const std::string& serverAddress, const std::string& credential);
    static std::optional<std::string> WinCredGetCredential(const std::string& serverAddress);
    static void WinCredEraseCredential(const std::string& serverAddress);
    static std::vector<std::string> WinCredListCredentials();

    // File backend helpers
    static std::filesystem::path GetFilePath();
    static std::string Protect(const std::string& plaintext);
    static std::string Unprotect(const std::string& cipherBase64);

    static void ModifyFileStore(const std::function<bool(nlohmann::json&)>& modifier);
    static nlohmann::json ReadFileStore();

    static void FileStoreCredential(const std::string& serverAddress, const std::string& credential);
    static std::optional<std::string> FileGetCredential(const std::string& serverAddress);
    static void FileEraseCredential(const std::string& serverAddress);
    static std::vector<std::string> FileListCredentials();
};

} // namespace wsl::windows::wslc::services
