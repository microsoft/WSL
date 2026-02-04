// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "pch.h"
#include "Errors.h"
#include "ExecutionArgs.h"
#include "ExecutionContext.h"

#include <string_view>

// Terminates the Context with some logging to indicate the location.
// Also returns from the current function.
#define WSLC_TERMINATE_CONTEXT_ARGS(_context_, _hr_, _ret_) \
    do \
    { \
        _context_.Terminate(_hr_, __FILE__, __LINE__); \
        return _ret_; \
    } while (0, 0)

// Terminates the Context named 'context' with some logging to indicate the location.
// Also returns from the current function.
#define WSLC_TERMINATE_CONTEXT(_hr_) WSLC_TERMINATE_CONTEXT_ARGS(context, _hr_, )

// Terminates the Context named 'context' with some logging to indicate the location.
// Also returns the specified value from the current function.
#define WSLC_TERMINATE_CONTEXT_RETURN(_hr_, _ret_) WSLC_TERMINATE_CONTEXT_ARGS(context, _hr_, _ret_)

// Returns if the context is terminated.
#define WSLC_RETURN_IF_TERMINATED(_context_) \
    if ((_context_).IsTerminated()) \
    { \
        return; \
    }

namespace wsl::windows::wslc
{
    struct Command;
}

namespace wsl::windows::wslc::task
{
    struct Task;
}

namespace wsl::windows::wslc::execution
{
    // The context within which all commands execute.
    // Contains arguments via Args.
    struct CLIExecutionContext : public wsl::windows::common::ExecutionContext
    {
        CLIExecutionContext() : wsl::windows::common::ExecutionContext(wsl::windows::common::Context::WslC) {}
        ~CLIExecutionContext() override = default;

        CLIExecutionContext(const CLIExecutionContext&) = delete;
        CLIExecutionContext(CLIExecutionContext&&) = delete;

        CLIExecutionContext& operator=(const CLIExecutionContext&) = delete;
        CLIExecutionContext& operator=(CLIExecutionContext&&) = delete;

        // The arguments given to execute.
        Args Args;

        // Returns a value indicating whether the context is terminated.
        bool IsTerminated() const
        {
            return m_isTerminated;
        }

        // Resets the context to a nonterminated state.
        void ResetTermination()
        {
            m_terminationHR = S_OK;
            m_isTerminated = false;
        }

        // Gets the HRESULT reason for the termination.
        HRESULT GetTerminationHR() const
        {
            return m_terminationHR;
        }

        // Set the context to the terminated state.
        void Terminate(HRESULT hr, std::string_view file = {}, size_t line = {});

        // Set the termination hr of the context.
        void SetTerminationHR(HRESULT hr);

        // Gets the executing command
        wsl::windows::wslc::Command* GetExecutingCommand() { return m_executingCommand; }

        // Sets the executing command
        void SetExecutingCommand(wsl::windows::wslc::Command* command) { m_executingCommand = command; }

    private:
        bool m_isTerminated = false;
        HRESULT m_terminationHR = S_OK;
        wsl::windows::wslc::Command* m_executingCommand = nullptr;
    };
}
