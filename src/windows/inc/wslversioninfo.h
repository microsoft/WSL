/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslversioninfo.h

Abstract:

    This file contains version definitions for WSL binaries.

--*/

#pragma once

#define VER_FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
#define VER_FILEOS VOS_NT_WINDOWS32
#define VER_PRODUCTNAME_STR "Windows Subsystem for Linux"
#define VER_PRODUCTVERSION_STR WSL_PACKAGE_VERSION
#define VER_FILEVERSION_STR WSL_PACKAGE_VERSION
#define VER_COMPANYNAME_STR "Microsoft Corporation"
#define VER_FILEVERSION WSL_PACKAGE_VERSION_MAJOR, WSL_PACKAGE_VERSION_MINOR, WSL_PACKAGE_VERSION_REVISION, 0

#ifdef _WINDLL

#define VER_FILETYPE VFT_DLL
#define VER_FILESUBTYPE VFT2_UNKNOWN

#else

#define VER_FILETYPE VFT_APP
#define VER_FILESUBTYPE VFT2_UNKNOWN

#endif

#ifdef DEBUG

#define VER_FILEFLAGS VS_FF_DEBUG | VS_FF_PRIVATEBUILD

#else

#ifdef WSL_OFFICIAL_BUILD

#define VER_FILEFLAGS 0

#else

#define VER_FILEFLAGS VS_FF_PRIVATEBUILD

#endif

#endif
