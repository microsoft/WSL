/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ICredentialStorage.cpp

Abstract:

    Factory for credential storage backends.

--*/

#include "ICredentialStorage.h"
#include "FileCredStorage.h"
#include "WinCredStorage.h"
#include "WSLCUserSettings.h"

namespace wsl::windows::wslc::services {

std::unique_ptr<ICredentialStorage> OpenCredentialStorage()
{
    auto backend = settings::User().Get<settings::Setting::CredentialStore>();
    if (backend == settings::CredentialStoreType::File)
    {
        return std::make_unique<FileCredStorage>();
    }

    return std::make_unique<WinCredStorage>();
}

} // namespace wsl::windows::wslc::services
