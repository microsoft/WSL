/*++

Copyright (c) Microsoft. All rights reserved.

--*/
#include "precomp.h"
#include "Argument.h"
#include "CLIExecutionContext.h"

using namespace wsl::shared;
using namespace wsl::windows::common;

namespace wsl::windows::wslc::execution {

HANDLE CLIExecutionContext::CreateCancelEvent()
{
    WI_ASSERT(!CancelEvent);
    CancelEvent.create(wil::EventOptions::ManualReset);
    return CancelEvent.get();
}

void CLIExecutionContext::ApplyGlobalEnvironmentOptions()
{
    // NoColor is environment-only and resolved before any output. Freezing it keeps the terminal
    // color state consistent for the entire invocation.
    Terminal.SetNoColor(GlobalArgs.GetValue<ArgType::NoColor>());
}

void CLIExecutionContext::ReportError(HRESULT result)
{
    std::wstring message;
    if (const auto& reported = ReportedError())
    {
        const auto strings = wslutil::ErrorToString(*reported);
        message = strings.Message.empty() ? strings.Code : strings.Message;
    }

    Terminal.Error(L"{}\n", Localization::MessageErrorCode(message, wslutil::ErrorCodeToString(result)));
}

void CLIExecutionContext::ClearError()
{
    m_error.reset();
}

} // namespace wsl::windows::wslc::execution
