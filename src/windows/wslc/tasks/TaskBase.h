// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
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

// The purpose of this model is to allow chaining of tasks and functions in a way that allows for short-circuiting
// if the context has been marked as terminated. For example, this allows for a task to be conditionally executed
// only if a prior task did not encounter an error and mark the context as terminated. In this way we avoid
// having to do constant checks for whether the context has been terminated in the body of each task, and can
// instead centralize the logic. It also makes for cleaner code when chaining multiple tasks together,
// as the chaining can be done in a single expression without needing to check the context in between each task.
// Example usage 1 (compact):
//    context << TaskA << TaskB << TaskC;
// Example usage 2 (more readable):
//    context
//        << TaskA
//        << TaskB
//        << TaskC;
//
// In order to support maintaining of data and state between tasks, the CLIExecutionContext has a Data member
// which is a map of data keys to arbitrary data values. These are defined in ExecutionContextData.h, and can
// be used in exactly the same way as getting argument data from the ArgMap in the context. This allows for tasks
// to share data and access the arguments without needing to have the data explicitly passed between them, and
// allows tasks to be more modular and shared between commands. The Arguments and Data being the same type of
// data structure keeps the interaction mode simple and consistent.

// Passes the context to the function if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, wsl::windows::wslc::task::Task::Func f);

// Passes the context to the task if it has not been terminated; returns the context.
CLIExecutionContext& operator<<(CLIExecutionContext& context, const wsl::windows::wslc::task::Task& task);
