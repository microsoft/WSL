/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleService.h

Abstract:

    This file contains the ConsoleService definition

--*/
#pragma once

#include <wslc.h>
#include <WSLCContainerLauncher.h>

namespace wsl::windows::wslc::services {
class ConsoleService
{
public:
    int AttachToCurrentConsole(wsl::windows::common::ClientRunningWSLCProcess&& process);
    static bool RelayInteractiveTty(wsl::windows::common::ClientRunningWSLCProcess& process, HANDLE tty, bool triggerRefresh = false);
    static void RelayNonTtyProcess(wil::unique_handle&& Stdin, wil::unique_handle&& Stdout, wil::unique_handle&& Stderr);
};
} // namespace wsl::windows::wslc::services
