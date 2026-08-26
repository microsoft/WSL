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

Microsoft::WRL::ComPtr<IWSLCContainer> ServiceContainerLauncher::Create(WSLCSession& Session)
{
    auto storage = CreateOptions();
    Microsoft::WRL::ComPtr<IWSLCContainer> container;
    Session.CreateContainerImpl(&storage.Options, &container);
    return container;
}

} // namespace wsl::windows::service::wslc
