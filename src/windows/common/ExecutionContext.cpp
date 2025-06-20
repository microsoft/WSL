// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ExecutionContext.h"
#include "wsleventschema.h"

using wsl::windows::common::ClientExecutionContext;
using wsl::windows::common::Context;
using wsl::windows::common::Error;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::ServiceExecutionContext;

thread_local ExecutionContext* g_currentContext = nullptr;
static bool g_enabled = false;
bool g_runningInService = false;
static HANDLE g_eventLog = nullptr;

void wsl::windows::common::EnableContextualizedErrors(bool service)
{
    WI_ASSERT(!g_enabled);
    g_enabled = true;
    g_runningInService = service;
}

ExecutionContext::ExecutionContext(Context context, FILE* warningsFile) noexcept :
    m_warningsFile(warningsFile), m_parent(g_currentContext), m_context(context)
{
    WI_ASSERT(g_currentContext == nullptr || g_currentContext->m_context < m_context);

    if (!g_enabled)
    {
        return;
    }

    g_currentContext = this;
}

ExecutionContext::~ExecutionContext()
{
    g_currentContext = m_parent;
    WI_ASSERT(!m_errorString.has_value());
}

ExecutionContext* ExecutionContext::Current()
{
    return g_currentContext;
}

void ExecutionContext::SetErrorStringImpl(std::wstring&& string)
{
    WI_ASSERT(!m_errorString.has_value());
    m_errorString = std::move(string);
}

bool ExecutionContext::CanCollectUserErrorMessage()
{
    if (g_runningInService)
    {
        if (m_parent != nullptr)
        {
            return m_parent->CanCollectUserErrorMessage();
        }
        else
        {
            // If we're running in a service and the root context isn't a service context,
            // then error messages cannot be reported.
            return false;
        }
    }
    else
    {
        return true;
    }
}

ULONGLONG ExecutionContext::CurrentContext() const noexcept
{
    ULONGLONG errorContext = m_context;
    for (const auto* e = m_parent; e != nullptr; e = e->m_parent)
    {
        errorContext |= static_cast<ULONGLONG>(e->m_context);
    }

    return errorContext;
}

void ExecutionContext::CollectErrorImpl(HRESULT result, ULONGLONG context, std::optional<std::wstring>&& message)
{
    WI_ASSERT(m_parent == nullptr);

    // Special case for an error being rethrown from a parent context.
    if (m_error.has_value() && m_error->Code == result && (context & m_error->Context) == context)
    {
        // This error has the same HRESULT that the one we already have and comes from a less specific context, drop.
        if (!m_error->Message.has_value() && message.has_value())
        {
            /* This is for the scenario where a specialized error message is sent after catching and rethrowing an error.
             * Example:
             * try
             * {
             *    something();
             * }
             * catch (...)
             * {
             *   THROW_HR_WITH_USER_ERROR(..., "Something failed: [...]");
             * }
             */

            m_error->Message = std::move(message);
        }

        return;
    }

    m_error.emplace(result, context, std::move(message));
}

void ExecutionContext::CollectError(HRESULT result)
{
    if (g_currentContext == nullptr)
    {
        return;
    }

    g_currentContext->CollectErrorImpl(result);
}

void ExecutionContext::CollectErrorImpl(HRESULT result)
{
    RootContext().CollectErrorImpl(result, CurrentContext(), std::move(m_errorString));

    m_errorString.reset();
}

void ExecutionContext::EmitUserWarning(const std::wstring& warning, const std::source_location& location)
{
    WSL_LOG(
        "UserWarning",
        TraceLoggingValue(location.file_name(), "FileName"),
        TraceLoggingValue(location.line(), "Line"),
        TraceLoggingValue(warning.c_str(), "Content"));

    if (!g_enabled)
    {
        return;
    }

    if (!CollectUserWarning(std::format(L"wsl: {}\n", warning)))
    {
        static std::atomic<bool> displayed = false;
        if (!displayed.exchange(true))
        {
            wsl::windows::common::notifications::DisplayWarningsNotification();
        }
    }

    if (g_eventLog)
    {
        auto* warningPtr = warning.c_str();
        LOG_IF_WIN32_BOOL_FALSE(ReportEventW(g_eventLog, EVENTLOG_WARNING_TYPE, 0, MSG_WARNING, nullptr, 1, 0, &warningPtr, nullptr));
    }
}

ExecutionContext& ExecutionContext::RootContext()
{
    if (m_parent == nullptr)
    {
        return *this;
    }
    else
    {
        return m_parent->RootContext();
    }
}

const std::optional<wsl::windows::common::Error>& ExecutionContext::ReportedError() const noexcept
{
    if (!m_error.has_value() && m_parent != nullptr)
    {
        return m_parent->ReportedError();
    }

    return m_error;
}

bool ExecutionContext::ShouldCollectErrorMessage()
{
    return g_currentContext != nullptr && g_currentContext->CanCollectUserErrorMessage();
}

bool ExecutionContext::CanCollectUserWarnings() const
{
    return m_warningsFile != nullptr;
}

bool ExecutionContext::CollectUserWarning(const std::wstring& warning)
{
    if (m_warningsFile != nullptr)
    {
        fputws(warning.c_str(), m_warningsFile);
        return true;
    }
    else if (m_parent != nullptr)
    {
        return m_parent->CollectUserWarning(warning);
    }
    else
    {
        return false;
    }
}

ClientExecutionContext::~ClientExecutionContext()
{
    if (m_outError.Message != nullptr)
    {
        CoTaskMemFree(m_outError.Message);
    }

    if (m_outError.Warnings != nullptr)
    {
        CollectUserWarning(m_outError.Warnings);
        CoTaskMemFree(m_outError.Warnings);
    }

    if (m_interactiveWarningsThread.joinable())
    {
        m_warningsPipeWrite.reset();
        m_interactiveWarningsThread.join();
    }
}

ClientExecutionContext::ClientExecutionContext(bool enableContextualizedErrors) : ExecutionContext(Service)
{
    WI_SetFlagIf(m_outError.Flags, LxssExecutionContextFlagsEnableContextualizedErrors, enableContextualizedErrors);
    WI_SetFlagIf(m_outError.Flags, LxssExecutionContextFlagsEnableUserWarnings, RootContext().CanCollectUserWarnings());
}

void ClientExecutionContext::CollectErrorImpl(HRESULT result)
{
    const auto errorContext = CurrentContext() | m_outError.Context;
    std::optional<std::wstring> message;
    if (m_outError.Message != nullptr)
    {
        WI_ASSERT(WI_IsFlagSet(m_outError.Flags, LxssExecutionContextFlagsEnableContextualizedErrors));
        message = std::wstring(m_outError.Message);
    }

    RootContext().CollectErrorImpl(result, errorContext, std::move(message));
}

void ClientExecutionContext::FlushWarnings()
{
    if (m_interactiveWarningsThread.joinable())
    {
        m_warningsPipeWrite.reset();
        m_interactiveWarningsThread.join();
    }

    if (m_outError.Warnings && CollectUserWarning(m_outError.Warnings))
    {
        CoTaskMemFree(m_outError.Warnings);
        m_outError.Warnings = nullptr;
    }
}

void ClientExecutionContext::EnableInteractiveWarnings()
{
    WI_ASSERT(!m_interactiveWarningsThread.joinable());

    wil::unique_handle read;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&read, &m_warningsPipeWrite, nullptr, 0));

    m_outError.WarningsPipe = HandleToULong(m_warningsPipeWrite.get());

    m_interactiveWarningsThread = std::thread([read = std::move(read)]() {
        try
        {
            wchar_t buffer[1024] = {0};

            DWORD bytesRead{};
            while (ReadFile(read.get(), buffer, sizeof(buffer) - sizeof(wchar_t), &bytesRead, nullptr) && bytesRead > 0)
            {
                const auto endIndex = bytesRead / sizeof(wchar_t);
                buffer[endIndex] = UNICODE_NULL;

                fwprintf(stderr, L"%ls", buffer);
            }
        }
        CATCH_LOG();
    });
}

ServiceExecutionContext::ServiceExecutionContext(LXSS_ERROR_INFO* outError) noexcept : ExecutionContext(Empty)
{
    if (outError != nullptr)
    {
        if (WI_IsFlagSet(outError->Flags, LxssExecutionContextFlagsEnableContextualizedErrors))
        {
            m_outError = outError;

            if (WI_IsFlagSet(outError->Flags, LxssExecutionContextFlagsEnableUserWarnings))
            {
                if (outError->WarningsPipe != 0)
                {
                    m_warningsPipe.reset(wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(outError->WarningsPipe)));
                }

                if (!m_warningsPipe)
                {
                    m_warningsString.emplace();
                }
            }
        }
    }
}

bool ServiceExecutionContext::CanCollectUserErrorMessage()
{
    return m_outError.has_value();
}

bool ServiceExecutionContext::CollectUserWarning(const std::wstring& warning)
{
    if (m_warningsPipe)
    {
        LOG_IF_WIN32_BOOL_FALSE(WriteFile(
            m_warningsPipe.get(), warning.c_str(), gsl::narrow_cast<DWORD>(warning.size() * sizeof(wchar_t)), nullptr, nullptr));

        return true;
    }

    if (m_warningsString.has_value())
    {
        *m_warningsString += warning;
        return true;
    }
    else
    {
        return false;
    }
}

ServiceExecutionContext::~ServiceExecutionContext()
{
    if (m_outError.has_value())
    {
        if (m_error.has_value())
        {
            m_outError.value()->Context = m_error->Context;

            if (m_error->Message.has_value())
            {
                m_outError.value()->Message = wil::make_unique_string<wil::unique_cotaskmem_string>(m_error->Message->c_str()).release();
            }
        }

        if (m_warningsString.has_value())
        {
            m_outError.value()->Warnings = wil::make_unique_string<wil::unique_cotaskmem_string>(m_warningsString->c_str()).release();
        }
    }
}

LXSS_ERROR_INFO* ClientExecutionContext::OutError() noexcept
{
    return &m_outError;
}

void wsl::windows::common::SetErrorMessage(std::wstring&& message)
{
    if (g_currentContext == nullptr || message.empty())
    {
        return; // no context to save the error to or empty message, ignore
    }

    g_currentContext->SetErrorStringImpl(std::move(message));
}

void wsl::windows::common::SetEventLog(HANDLE eventLog)
{
    WI_ASSERT(!g_eventLog);

    g_eventLog = eventLog;
}
