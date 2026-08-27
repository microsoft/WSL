/*++

Copyright (c) Microsoft. All rights reserved.

--*/
#include "precomp.h"
#include "Argument.h"
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
    if (GlobalArgs.Contains(ArgType::NoColor))
    {
        Reporter.SetNoColor(true);
    }
}

} // namespace wsl::windows::wslc::execution
