// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "context.h"
#include "executionargs.h"
#include <string>
#include <string_view>

using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::workflow
{
    // A task in the workflow.
    struct WorkflowTask
    {
        using Func = void (*)(CLIExecutionContext&);

        WorkflowTask(Func f) : m_isFunc(true), m_func(f) {}
        WorkflowTask(std::wstring_view name, bool executeAlways = false) : m_name(name), m_executeAlways(executeAlways) {}

        virtual ~WorkflowTask() = default;

        WorkflowTask(const WorkflowTask&) = default;
        WorkflowTask& operator=(const WorkflowTask&) = default;

        WorkflowTask(WorkflowTask&&) = default;
        WorkflowTask& operator=(WorkflowTask&&) = default;

        bool operator==(const WorkflowTask& other) const;

        virtual void operator()(CLIExecutionContext& context) const;

        const std::wstring& GetName() const { return m_name; }
        bool IsFunction() const { return m_isFunc; }
        Func Function() const { return m_func; }
        bool ExecuteAlways() const { return m_executeAlways; }
        void Log() const;

    private:
        bool m_isFunc = false;
        Func m_func = nullptr;
        std::wstring m_name;
        bool m_executeAlways = false;
    };

    // Helper to report exceptions and return the HRESULT.
    // If context is null, no output will be attempted.
    HRESULT HandleException(CLIExecutionContext* context, std::exception_ptr exception);

    // Helper to report exceptions and return the HRESULT.
    HRESULT HandleException(CLIExecutionContext& context, std::exception_ptr exception);
}

// Passes the context to the function if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, wsl::windows::wslc::workflow::WorkflowTask::Func f);

// Passes the context to the task if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, const wsl::windows::wslc::workflow::WorkflowTask& task);
