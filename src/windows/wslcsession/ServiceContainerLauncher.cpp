/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceContainerLauncher.cpp

Abstract:

    Implements the service-side container launcher.

--*/

#include "precomp.h"
#include "ServiceContainerLauncher.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

Microsoft::WRL::ComPtr<WSLCContainer> ServiceContainerLauncher::Create(WSLCSession& Session)
{
    auto storage = CreateOptions();
    return Session.CreateContainerImpl(&storage.Options);
}

} // namespace wsl::windows::service::wslc
