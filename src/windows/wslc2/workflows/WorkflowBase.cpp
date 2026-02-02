// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "WorkflowBase.h"
#include <wil/token_helpers.h>
#include <wil/result_macros.h>

using namespace wsl::shared;
using namespace wsl::windows::common;
using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::execution;
using namespace std::string_literals;
using namespace winrt::Windows::Foundation;

namespace wsl::windows::wslc::workflow
{
    namespace
    {
        bool IsRunningAsAdmin()
        {
            return wil::test_token_membership(nullptr, SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);
        }
    }

    bool WorkflowTask::operator==(const WorkflowTask& other) const
    {
        if (m_isFunc && other.m_isFunc)
        {
            return m_func == other.m_func;
        }
        else if (!m_isFunc && !other.m_isFunc)
        {
            return m_name == other.m_name;
        }
        else
        {
            return false;
        }
    }

    void WorkflowTask::operator()(CLIExecutionContext& context) const
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_isFunc);
        m_func(context);
    }

    void WorkflowTask::Log() const
    {
        // TODO: Log
    }

    HRESULT HandleException(CLIExecutionContext* context, std::exception_ptr exception)
    {
        try
        {
            std::rethrow_exception(exception);
        }
        catch (...)
        {
            // Using WSL shared utility to get the HRESULT from the caught exception.
            // CLIExecutionContext is a derived class of wsl::windows::common::ExecutionContext.
            auto result = wil::ResultFromCaughtException();
            if (FAILED(result))
            {
                if (const auto& reported = context->ReportedError())
                {
                    auto strings = wslutil::ErrorToString(*reported);
                    auto errorMessage = strings.Message.empty() ? strings.Code : strings.Message;
                    wslutil::PrintMessage(Localization::MessageErrorCode(errorMessage, wslutil::ErrorCodeToString(result)), stderr);
                }
                else
                {
                    // Fallback for errors without context
                    wslutil::PrintMessage(Localization::MessageErrorCode("", wslutil::ErrorCodeToString(result)), stderr);
                }
            }

            return result;
        }

        return E_UNEXPECTED;
    }

    HRESULT HandleException(CLIExecutionContext& context, std::exception_ptr exception)
    {
        return HandleException(&context, exception);
    }

    void EnsureRunningAsAdmin(CLIExecutionContext& context)
    {
        if (!IsRunningAsAdmin())
        {
            wslutil::PrintMessage(Localization::WSLCCLI_CommandRequiresAdmin(), stderr);
            WSLC_TERMINATE_CONTEXT(WSLC_CLI_ERROR_COMMAND_REQUIRES_ADMIN);
        }
    }

    void ReportExecutionStage::operator()(CLIExecutionContext& context) const
    {
        context.SetExecutionStage(m_stage);
    }
}

CLIExecutionContext& operator<<(CLIExecutionContext& context, wsl::windows::wslc::workflow::WorkflowTask::Func f)
{
    return (context << wsl::windows::wslc::workflow::WorkflowTask(f));
}

CLIExecutionContext& operator<<(CLIExecutionContext& context, const wsl::windows::wslc::workflow::WorkflowTask& task)
{
    if (!context.IsTerminated() || task.ExecuteAlways())
    {
        task.Log();
        task(context);
    }

    return context;
}
