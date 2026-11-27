/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WinCredStorage.h

Abstract:

    Windows Credential Manager credential storage backend.

--*/
#pragma once

#include "ICredentialStorage.h"

namespace wsl::windows::wslc::services {

class WinCredStorage final : public ICredentialStorage
{
public:
    void Store(const std::string& serverAddress, const std::string& username, const std::string& secret) override;
    std::pair<std::string, std::string> Get(const std::string& serverAddress) override;
    void Erase(const std::string& serverAddress) override;
    std::vector<std::wstring> List() override;

private:
    static std::wstring TargetName(const std::string& serverAddress);
};

} // namespace wsl::windows::wslc::services
