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

//
// N.B. The 'P' flag preserves Argv[0]. The 'F' flag (fix-binary) opens the interpreter at
//      registration time, making it available across mount namespaces and chroot environments.
//      WSL1 (lxcore) does not support the 'F' flag, so it must only be used in WSL2 (VM) paths.
//

#define BINFMT_INTEROP_REGISTRATION_STRING(Name) ":" Name ":M::MZ::" LX_INIT_PATH ":P"
#define BINFMT_INTEROP_REGISTRATION_STRING_VM(Name) ":" Name ":M::MZ::" LX_INIT_PATH ":FP"

int CreateNtProcess(int Argc, char* Argv[]);
