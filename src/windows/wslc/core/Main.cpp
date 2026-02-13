/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Main.cpp

Abstract:

    Main program entry point.

--*/
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "Core.h"

int wmain(int argc, wchar_t const** argv)
{
    return wsl::windows::wslc::CoreMain(argc, argv);
}
