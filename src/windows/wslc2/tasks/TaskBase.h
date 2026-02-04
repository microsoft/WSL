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

namespace wsl::windows::wslc::task
{
    struct Task
    {
        using Func = void (*)(CLIExecutionContext&);

        Task(Func f) : m_isFunc(true), m_func(f) {}
        Task(std::wstring_view name) : m_name(name) {}

        virtual ~Task() = default;

        Task(const Task&) = default;
        Task& operator=(const Task&) = default;

        Task(Task&&) = default;
        Task& operator=(Task&&) = default;

        bool operator==(const Task& other) const;

        virtual void operator()(CLIExecutionContext& context) const;

        const std::wstring& GetName() const { return m_name; }
        bool IsFunction() const { return m_isFunc; }
        Func Function() const { return m_func; }

    private:
        bool m_isFunc = false;
        Func m_func = nullptr;
        std::wstring m_name;
    };

    // Helper to report exceptions and return the HRESULT.
    // If context is null, no output will be attempted.
    HRESULT HandleException(CLIExecutionContext* context, std::exception_ptr exception);

    // Helper to report exceptions and return the HRESULT.
    HRESULT HandleException(CLIExecutionContext& context, std::exception_ptr exception);
}

// Passes the context to the function if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, wsl::windows::wslc::task::Task::Func f);

// Passes the context to the task if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, const wsl::windows::wslc::task::Task& task);
