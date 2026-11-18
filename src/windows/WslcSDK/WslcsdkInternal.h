/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcsdkInternal.h

Abstract:

    This file contains WSL Container SDK entry points that are only used by the WinRT wrapper.

--*/
#pragma once
#include "wslcsdk.h"

// Same as WslcStartContainer, except that when activateIOCallbacks is false the init process IO callbacks are registered
// but not delivered until WslcActivateContainerIOCallbacks is called.
STDAPI WslcStartContainerInternal(_In_ WslcContainer container, _In_ WslcContainerStartFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage, _In_ BOOL activateIOCallbacks);

STDAPI WslcActivateContainerIOCallbacks(_In_ WslcContainer container);
