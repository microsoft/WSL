/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ICredentialStorage.h

Abstract:

    Interface for credential storage backends.

--*/
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::wslc::services {

// Abstract interface for credential storage backends (WinCred, file-based, etc.).
struct ICredentialStorage
{
    virtual ~ICredentialStorage() = default;

    virtual void Store(const std::string& serverAddress, const std::string& credential) = 0;
    virtual std::optional<std::string> Get(const std::string& serverAddress) = 0;
    virtual void Erase(const std::string& serverAddress) = 0;
    virtual std::vector<std::wstring> List() = 0;
};

// Returns the credential storage implementation based on user configuration.
std::unique_ptr<ICredentialStorage> OpenCredentialStorage();

} // namespace wsl::windows::wslc::services
