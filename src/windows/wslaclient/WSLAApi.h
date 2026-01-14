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