/*++

Copyright (c) Microsoft. All rights reserved.

--*/
#include "precomp.h"
#include "Argument.h"
#include "CLIExecutionContext.h"

using namespace wsl::shared;
using namespace wsl::windows::common;

namespace wsl::windows::wslc::execution {

using namespace wsl::windows::wslc::cli;

HANDLE CLIExecutionContext::CreateCancelEvent(CancellationMode mode)
{
    WI_ASSERT(!CancelEvent);
    THROW_HR_IF(E_INVALIDARG, mode == CancellationMode::None);
    // Publish supported stages before creating handles so early requests can be replayed.
    m_cancellationMode.store(mode);
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
    WI_ASSERT(m_cancellationMode.load() == CancellationMode::GracefulAndForce);
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
    ULONG supportedRequests = 0;
    switch (m_cancellationMode.load())
    {
    case CancellationMode::None:
        return false;
    case CancellationMode::Graceful:
        supportedRequests = 1;
        break;
    case CancellationMode::GracefulAndForce:
        supportedRequests = 2;
        break;
    }

    const auto cancellationCount = CancellationCount.fetch_add(1) + 1;
    if (cancellationCount > supportedRequests)
    {
        return false;
    }

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

void CLIExecutionContext::ApplyTerminalOptions()
{
    // NoColor is environment-only and resolved before any output. Freezing it keeps the terminal
    // color state consistent for the entire invocation.
    Terminal.SetNoColor(Args.GetValue<ArgType::NoColor>());
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

bool CLIExecutionContext::CollectUserWarning(const std::wstring& warning)
{
    // The message already carries the "wsl: " prefix and trailing newline, so it is written verbatim.
    Terminal.Warn(L"{}", warning);
    return true;
}

} // namespace wsl::windows::wslc::execution
