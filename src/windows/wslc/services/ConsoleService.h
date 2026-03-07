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

namespace wsl::windows::wslc::services {
class ConsoleService
{
public:
    int AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLAProcess&& process);
    static bool RelayInteractiveTty(wsl::windows::common::ClientRunningWSLAProcess& process, HANDLE tty, bool triggerRefresh = false);
    static void RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr);
};
} // namespace wsl::windows::wslc::services
