/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    TaskBase.cpp

Abstract:

    Implementation of task execution logic.

--*/
#pragma once
#include "pch.h"
#include "TaskBase.h"
#include <wil/result_macros.h>

using namespace wsl::windows::common;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::task
{
    bool Task::operator==(const Task& other) const
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

    void Task::operator()(CLIExecutionContext& context) const
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_isFunc);
        m_func(context);
    }

    HRESULT HandleException(CLIExecutionContext* context, std::exception_ptr exception)
    {
        try
        {
            std::rethrow_exception(exception);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();

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
                    ////WSLC_LOG(Fail, Error, << wslutil::ErrorCodeToString(result) << L": " << errorMessage);
                }
                else
                {
                    // Fallback for errors without context
                    wslutil::PrintMessage(Localization::MessageErrorCode("", wslutil::ErrorCodeToString(result)), stderr);
                    ////WSLC_LOG(Fail, Error, << wslutil::ErrorCodeToString(result) << L": <Unknown>");
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
}

CLIExecutionContext& operator<<(CLIExecutionContext& context, wsl::windows::wslc::task::Task::Func f)
{
    return (context << wsl::windows::wslc::task::Task(f));
}

CLIExecutionContext& operator<<(CLIExecutionContext& context, const wsl::windows::wslc::task::Task& task)
{
    // This is a common termination check automatically executed before every task.
    // If the context has been terminated, we will skip executing this task.
    if (!context.IsTerminated())
    {
        task(context);
    }

    return context;
}
