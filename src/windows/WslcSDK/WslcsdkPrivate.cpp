/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDKPrivate.cpp

Abstract:

    This file contains the private WSL Container SDK implementations.

--*/
#include "precomp.h"

#include "wslcsdkprivate.h"


WSLC_SESSION_OPTIONS_INTERNAL* GetInternalOptions(WslcSessionSettings* settings)
{
    return reinterpret_cast<WSLC_SESSION_OPTIONS_INTERNAL*>(settings);
}

WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* GetInternalOptions(WslcProcessSettings* settings)
{
    return reinterpret_cast<WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL*>(settings);
}

WSLC_CONTAINER_OPTIONS_INTERNAL* GetInternalOptions(WslcContainerSettings* settings)
{
    return reinterpret_cast<WSLC_CONTAINER_OPTIONS_INTERNAL*>(settings);
}
