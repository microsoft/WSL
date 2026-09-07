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

HANDLE CLIExecutionContext::CreateForceCancelEvent()
{
    WI_ASSERT(CancelEvent);
    WI_ASSERT(!ForceCancelEvent);
    ForceCancelEvent.create(wil::EventOptions::ManualReset);
    return ForceCancelEvent.get();
}

bool CLIExecutionContext::RecordCancellationRequest() noexcept
{
    const auto cancellationCount = CancellationCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cancellationCount == 1)
    {
        return CancelEvent && SetEvent(CancelEvent.get());
    }
    if (cancellationCount == 2)
    {
        return ForceCancelEvent && SetEvent(ForceCancelEvent.get());
    }

    return false;
}

void CLIExecutionContext::ApplyGlobalEnvironmentOptions()
{
    // NoColor is environment-only and resolved before any output. Freezing it keeps the terminal
    // color state consistent for the entire invocation.
    Terminal.SetNoColor(GlobalArgs.GetValue<ArgType::NoColor>());
}

} // namespace wsl::windows::wslc::execution
