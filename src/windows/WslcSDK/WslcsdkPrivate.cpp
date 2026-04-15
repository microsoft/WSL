/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDKPrivate.cpp

Abstract:

    This file contains the private WSL Container SDK implementations.

--*/
#include "precomp.h"

#include "wslcsdkprivate.h"

WslcSessionOptionsInternal* GetInternalType(WslcSessionSettings* settings)
{
    return reinterpret_cast<WslcSessionOptionsInternal*>(settings);
}

WslcContainerProcessOptionsInternal* GetInternalType(WslcProcessSettings* settings)
{
    return reinterpret_cast<WslcContainerProcessOptionsInternal*>(settings);
}

WslcContainerOptionsInternal* GetInternalType(WslcContainerSettings* settings)
{
    return reinterpret_cast<WslcContainerOptionsInternal*>(settings);
}

const WslcContainerOptionsInternal* GetInternalType(const WslcContainerSettings* settings)
{
    return reinterpret_cast<const WslcContainerOptionsInternal*>(settings);
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
