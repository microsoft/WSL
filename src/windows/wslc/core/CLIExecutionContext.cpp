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

bool CLIExecutionContext::ConfirmPrune(std::wstring_view warning)
{
    if (Args.GetValue<ArgType::Force>())
    {
        return true;
    }

    return Terminal.Confirm(std::format(L"{}\n{}", warning, wsl::shared::Localization::WSLCCLI_PruneConfirmPrompt()));
}

void CLIExecutionContext::ApplyGlobalEnvironmentOptions()
{
    // NoColor is environment-only and resolved before any output. Freezing it keeps the terminal
    // color state consistent for the entire invocation.
    Terminal.SetNoColor(GlobalArgs.GetValue<ArgType::NoColor>());
}

} // namespace wsl::windows::wslc::execution
