/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLA.h

Abstract:

    This file contains the WSLA api definitions.

--*/
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* WslVirtualMachineHandle;

enum WslMountFlags
{
    WslMountFlagsNone = 0,
    WslMountFlagsChroot = 1,
    WslMountFlagsWriteableOverlayFs = 2,
};

enum WslFdType
{
    WslFdTypeDefault = 0,
    WslFdTypeTerminalInput = 1,
    WslFdTypeTerminalOutput = 2,
    WslFdTypeLinuxFileInput = 4,
    WslFdTypeLinuxFileOutput = 8,
    WslFdTypeLinuxFileAppend = 16,
    WslFdTypeLinuxFileCreate = 32,
    WslFdTypeTerminalControl = 64,
};

enum WslProcessState
{
    WslProcessStateUnknown,
    WslProcessStateRunning,
    WslProcessStateExited,
    WslProcessStateSignaled
};

enum WslInstallComponent
{
    WslInstallComponentNone = 0,
    WslInstallComponentVMPOC = 1,
    WslInstallComponentWslOC = 2,
    WslInstallComponentWslPackage = 4,
};

HRESULT WslQueryMissingComponents(enum WslInstallComponent* Components);

typedef void (*WslInstallCallback)(enum WslInstallComponent, uint64_t, uint64_t, void*);

HRESULT WslInstallComponents(enum WslInstallComponent Components, WslInstallCallback ProgressCallback, void* Context);

// Used for testing until the package is published.
HRESULT WslSetPackageUrl(LPCWSTR Url);

#ifdef __cplusplus
}
#endif