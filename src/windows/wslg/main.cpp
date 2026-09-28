/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entry point for wslg.exe.

--*/

#include "precomp.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return wsl::windows::common::WslClient::Main(GetCommandLineW());
}
