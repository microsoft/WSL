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
#include <ConsoleState.h>
#include "Terminal.h"

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::cli;
class ConsoleService
{
public:
    static int AttachToCurrentConsole(
        Terminal& terminal, wsl::windows::common::ConsoleState& console, wsl::windows::common::ClientRunningWSLCProcess&& process, bool triggerRefresh = false);
    static bool RelayInteractiveTty(
        wsl::windows::common::ConsoleState& console, wsl::windows::common::ClientRunningWSLCProcess& process, HANDLE tty, bool triggerRefresh = false);
    static void RelayNonTtyProcess(
        wsl::windows::common::io::HandleWrapper&& Stdin,
        wsl::windows::common::io::HandleWrapper&& Stdout,
        wsl::windows::common::io::HandleWrapper&& Stderr);
    static void RelayNonTtyProcess(
        wsl::windows::common::io::HandleWrapper&& Stdin,
        wsl::windows::common::io::HandleWrapper&& Stdout,
        wsl::windows::common::io::HandleWrapper&& Stderr,
        HANDLE Output,
        HANDLE Error);
};
} // namespace wsl::windows::wslc::services
