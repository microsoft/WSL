/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslinfo.h

Abstract:

    This file contains wslinfo function declarations.

--*/

#pragma once

#define WSLINFO_NAME "wslinfo"

#define WSLINFO_MSAL_PROXY_PATH "--msal-proxy-path"
#define WSLINFO_NETWORKING_MODE "--networking-mode"
#define WSLINFO_WSL_VERSION "--version"
#define WSLINFO_WSL_VERSION_LEGACY "--wsl-version"
#define WSLINFO_WSL_VMID "--vm-id"
#define WSLINFO_WSL_HELP "--help"
#define WSLINFO_NO_NEWLINE 'n'

int WslInfoEntry(int Argc, char* Argv[]);
