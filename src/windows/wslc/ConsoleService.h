/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleService.h

Abstract:

    This file contains the ConsoleService definition

--*/
#pragma once

#include <wslaservice.h>
#include <WSLAContainerLauncher.h>

namespace wslc::services {
class ConsoleService
{
public:
    int AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLAProcess&& process);
};
} // namespace wslc::services
