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

bool CLIExecutionContext::RecordCancellationRequest() noexcept
{
    const auto cancellationCount = CancellationCount.fetch_add(1, std::memory_order_relaxed) + 1;
    return cancellationCount == 1 && CancelEvent && SetEvent(CancelEvent.get());
}

void CLIExecutionContext::ApplyGlobalEnvironmentOptions()
{
    // NoColor is environment-only and resolved before any output. Freezing it keeps the terminal
    // color state consistent for the entire invocation.
    Terminal.SetNoColor(GlobalArgs.GetValue<ArgType::NoColor>());
}

} // namespace wsl::windows::wslc::execution
