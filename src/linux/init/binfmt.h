/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    binfmt.h

Abstract:

    This file contains declarations for the Windows interop binfmt interpreter.

--*/

#pragma once

//
// Name of the WSL binfmt_misc intrepreter.
//

#define LX_INIT_BINFMT_NAME "WSLInterop"

//
// Name of the WSL 'late' binfmt_misc intrepreter.
// This name is used by the wsl-binfmt systemd unit which
// registers the interpreter a second time after systemd-binfmt to make sure
// that wsl's interpreter is always registered last.
//

#define LX_INIT_BINFMT_NAME_LATE "WSLInterop-late"
#define BINFMT_MISC_MOUNT_TARGET "/proc/sys/fs/binfmt_misc"
#define BINFMT_MISC_REGISTER_FILE BINFMT_MISC_MOUNT_TARGET "/register"
#define BINFMT_INTEROP_REGISTRATION_STRING(Name) ":" Name ":M::MZ::" LX_INIT_PATH ":P"

int CreateNtProcess(int Argc, char* Argv[]);
