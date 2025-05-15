/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entry point for wsl.exe.

--*/

#include "precomp.h"

int __cdecl wmain()
{
    return wsl::windows::common::WslClient::Main(GetCommandLineW());
}
