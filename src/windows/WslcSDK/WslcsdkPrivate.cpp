/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDKPrivate.cpp

Abstract:

    This file contains the private WSL Container SDK implementations.

--*/
#include "precomp.h"

#include "wslcsdkprivate.h"


WSLC_SESSION_OPTIONS_INTERNAL* GetInternalType(WslcSessionSettings* settings)
{
    return reinterpret_cast<WSLC_SESSION_OPTIONS_INTERNAL*>(settings);
}

WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* GetInternalType(WslcProcessSettings* settings)
{
    return reinterpret_cast<WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL*>(settings);
}

WSLC_CONTAINER_OPTIONS_INTERNAL* GetInternalType(WslcContainerSettings* settings)
{
    return reinterpret_cast<WSLC_CONTAINER_OPTIONS_INTERNAL*>(settings);
}

WslcSessionImpl* GetInternalType(WslcSession handle)
{
    return reinterpret_cast<WslcSessionImpl*>(handle);
}

WslcContainerImpl* GetInternalType(WslcContainer handle)
{
    return reinterpret_cast<WslcContainerImpl*>(handle);
}

WslcProcessImpl* GetInternalType(WslcProcess handle)
{
    return reinterpret_cast<WslcProcessImpl*>(handle);
}
