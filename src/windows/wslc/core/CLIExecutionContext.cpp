/*++

Copyright (c) Microsoft. All rights reserved.

--*/
#include "precomp.h"
#include "CLIExecutionContext.h"

namespace wsl::windows::wslc::execution {

HANDLE CLIExecutionContext::CreateCancelEvent()
{
    WI_ASSERT(!CancelEvent);
    CancelEvent.create(wil::EventOptions::ManualReset);
    return CancelEvent.get();
}

// This method should be idempotent.
void CLIExecutionContext::ApplyGlobalOptions()
{
    // TODO: Add per-global side effects here as features land.
}

} // namespace wsl::windows::wslc::execution
