/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Task.h

Abstract:

    Declaration of a task for function composition and chaining.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include <string>
#include <string_view>
#include <functional>

using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::task {

struct Task
{
    using Func = std::function<void(CLIExecutionContext&)>;

    Task(void (*f)(CLIExecutionContext&)) : m_func(f)
    {
    }

    Task(Func f) : m_func(std::move(f))
    {
    }

    Task(const Task&) = default;
    Task& operator=(const Task&) = default;

    void operator()(CLIExecutionContext& context) const
    {
        m_func(context);
    }

private:
    Func m_func;
};

inline CLIExecutionContext& operator<<(CLIExecutionContext& context, const Task& task)
{
    return task(context), context;
}

inline CLIExecutionContext& operator<<(CLIExecutionContext& context, void (*f)(CLIExecutionContext&))
{
    return context << Task(f);
}

} // namespace wsl::windows::wslc::task
