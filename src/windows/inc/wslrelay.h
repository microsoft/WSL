/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslrelay.h

Abstract:

    Header file containing command line arguments for wslrelay.exe.

--*/

#pragma once

namespace wslrelay {
enum RelayMode
{
    Invalid = -1,
    DebugConsole,
    PortRelay,
    KdRelay
};

LPCWSTR const binary_name = L"wslrelay.exe";
LPCWSTR const mode_option = L"--mode";
LPCWSTR const handle_option = L"--handle";
LPCWSTR const vm_id_option = L"--vm-id";
LPCWSTR const pipe_option = L"--pipe";
LPCWSTR const exit_event_option = L"--exit-event";
LPCWSTR const port_option = L"--port";
LPCWSTR const disable_telemetry_option = L"--disable-telemetry";
LPCWSTR const connect_pipe_option = L"--connect-pipe";
} // namespace wslrelay