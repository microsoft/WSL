/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxfsshares.h

Abstract:

    This file contains share names and paths that are consumed by both init
    and the WSL service.

--*/

#pragma once

typedef struct _LXSS_SHARED_DIRECTORY
{
    const char* Name;
    const char* MountPoint;
} LXSS_SHARED_DIRECTORY, *PLXSS_SHARED_DIRECTORY;

#define LXSS_LIB_PREFIX "/usr/lib/wsl"
#define LXSS_LIB_PATH LXSS_LIB_PREFIX "/lib"
#define LXSS_GPU_DRIVERS_SHARE "drivers"
#define LXSS_GPU_LIB_SHARE "lib"
#define LXSS_GPU_INBOX_LIB_SHARE LXSS_GPU_LIB_SHARE "_inbox"
#define LXSS_GPU_PACKAGED_LIB_SHARE LXSS_GPU_LIB_SHARE "_packaged"

//
// Shared directories for GPU compute support.
//

constexpr LXSS_SHARED_DIRECTORY g_gpuShares[] = {{LXSS_GPU_DRIVERS_SHARE, LXSS_LIB_PREFIX "/drivers"}, {LXSS_GPU_LIB_SHARE, LXSS_LIB_PATH}};
