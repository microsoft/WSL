/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    binfmt.h

Abstract:

    This file contains declarations for the Windows interop binfmt interpreter.

--*/

#pragma once

//
// Name of the WSL binfmt_misc interpreter.
//

#define LX_INIT_BINFMT_NAME "WSLInterop"
#define BINFMT_MISC_MOUNT_TARGET "/proc/sys/fs/binfmt_misc"
#define BINFMT_MISC_REGISTER_FILE BINFMT_MISC_MOUNT_TARGET "/register"
#define BINFMT_INTEROP_REGISTRATION_STRING(Name) ":" Name ":M::MZ::" LX_INIT_PATH ":P"

int CreateNtProcess(int Argc, char* Argv[]);
