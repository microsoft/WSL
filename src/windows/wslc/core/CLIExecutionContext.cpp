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
    m_cancelEventHandle.store(CancelEvent.get());
    if (CancellationCount.load() >= 1)
    {
        THROW_LAST_ERROR_IF(!SetEvent(CancelEvent.get()));
    }

    return CancelEvent.get();
}

HANDLE CLIExecutionContext::CreateForceCancelEvent()
{
    WI_ASSERT(CancelEvent);
    WI_ASSERT(!ForceCancelEvent);
    ForceCancelEvent.create(wil::EventOptions::ManualReset);
    m_forceCancelEventHandle.store(ForceCancelEvent.get());
    if (CancellationCount.load() >= 2)
    {
        THROW_LAST_ERROR_IF(!SetEvent(ForceCancelEvent.get()));
    }

    return ForceCancelEvent.get();
}

bool CLIExecutionContext::RecordCancellationRequest() noexcept
{
    const auto cancellationCount = CancellationCount.fetch_add(1) + 1;
    if (cancellationCount == 1)
    {
        const auto cancelEvent = m_cancelEventHandle.load();
        return !cancelEvent || SetEvent(cancelEvent);
    }
    if (cancellationCount == 2)
    {
        const auto forceCancelEvent = m_forceCancelEventHandle.load();
        return !forceCancelEvent || SetEvent(forceCancelEvent);
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
