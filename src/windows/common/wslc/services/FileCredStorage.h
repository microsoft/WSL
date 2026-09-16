/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    FileCredStorage.h

Abstract:

    DPAPI-encrypted JSON file credential storage backend.

--*/
#pragma once

#include "ICredentialStorage.h"
#include "JsonUtils.h"

namespace wsl::windows::wslc::services {

inline constexpr int CredentialFileVersion = 1;

struct CredentialEntry
{
    std::string UserName;
    std::string Secret;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CredentialEntry, UserName, Secret);
};

struct CredentialFile
{
    int Version = CredentialFileVersion;
    std::map<std::string, CredentialEntry> Credentials;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(CredentialFile, Version, Credentials);
};

class FileCredStorage final : public ICredentialStorage
{
public:
    void Store(const std::string& serverAddress, const std::string& username, const std::string& secret) override;
    std::pair<std::string, std::string> Get(const std::string& serverAddress) override;
    void Erase(const std::string& serverAddress) override;
    std::vector<std::wstring> List() override;
};

} // namespace wsl::windows::wslc::services
