/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceContainerLauncher.h

Abstract:

    Defines the service-side container launcher.

--*/

#pragma once

#include "WSLCContainerLauncher.h"

namespace wsl::windows::service::wslc {

class WSLCSession;

class ServiceContainerLauncher : public common::WSLCContainerLauncher
{
public:
    NON_COPYABLE(ServiceContainerLauncher);
    NON_MOVABLE(ServiceContainerLauncher);
    using WSLCContainerLauncher::WSLCContainerLauncher;

    Microsoft::WRL::ComPtr<IWSLCContainer> Create(WSLCSession& Session);
};

} // namespace wsl::windows::service::wslc
