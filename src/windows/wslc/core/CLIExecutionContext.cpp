/*++

Copyright (c) Microsoft. All rights reserved.

--*/
#include "precomp.h"
#include "CLIExecutionContext.h"

namespace wsl::windows::wslc::execution {

HANDLE CreateCancelEvent()
{
    WI_ASSERT(!CancelEvent);
    CancelEvent.create(wil::EventOptions::ManualReset);
    return CancelEvent.get();
}

// This method should be idempotent.
void CLIExecutionContext::ApplyGlobalOptions()
{
    // Stub: plumbing only. Add per-global side effects here as features land.
    //
    // Example shape:
    //   if (GlobalArgs.Contains(ArgType::NoColor)) { /* disable VT color */ }
    //   if (GlobalArgs.Contains(ArgType::Debug))   { /* enable debug sink */ }
}

} // namespace wsl::windows::wslc::execution
