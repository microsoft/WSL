/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    FileCredStorage.h

Abstract:

    DPAPI-encrypted JSON file credential storage backend.

--*/
#pragma once

#include "ICredentialStorage.h"

namespace wsl::windows::wslc::services {

class FileCredStorage final : public ICredentialStorage
{
public:
    void Store(const std::string& serverAddress, const std::string& credential) override;
    std::optional<std::string> Get(const std::string& serverAddress) override;
    void Erase(const std::string& serverAddress) override;
    std::vector<std::wstring> List() override;
};

} // namespace wsl::windows::wslc::services
