// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#include "pch.h"
#include "errors.h"
#include "executionargs.h"
#include "ExecutionContext.h"

#include <string_view>

#define WSLC_CATCH_RESULT_EXCEPTION_STORE(exceptionHR)   catch (const wil::ResultException& re) { exceptionHR = re.GetErrorCode(); }
#define WSLC_CATCH_HRESULT_EXCEPTION_STORE(exceptionHR)   catch (const winrt::hresult_error& hre) { exceptionHR = hre.code(); }
#define WSLC_CATCH_COMMAND_EXCEPTION_STORE(exceptionHR)   catch (const ::AppInstaller::CLI::CommandException&) { exceptionHR = WSLC_CLI_ERROR_INVALID_CL_ARGUMENTS; }
#define WSLC_CATCH_STD_EXCEPTION_STORE(exceptionHR, genericHR)   catch (const std::exception&) { exceptionHR = genericHR; }
#define WSLC_CATCH_ALL_EXCEPTION_STORE(exceptionHR, genericHR)   catch (...) { exceptionHR = genericHR; }
#define WSLC_CATCH_STORE(exceptionHR, genericHR) \
        WSLC_CATCH_RESULT_EXCEPTION_STORE(exceptionHR) \
        WSLC_CATCH_HRESULT_EXCEPTION_STORE(exceptionHR) \
        WSLC_CATCH_COMMAND_EXCEPTION_STORE(exceptionHR) \
        WSLC_CATCH_STD_EXCEPTION_STORE(exceptionHR, genericHR) \
        WSLC_CATCH_ALL_EXCEPTION_STORE(exceptionHR, genericHR)

namespace wsl::windows::wslc
{
    struct Command;
}

namespace wsl::windows::wslc::workflow
{
    struct WorkflowTask;
    enum class ExecutionStage : uint32_t;
}

namespace wsl::windows::wslc::execution
{
    // bit masks used as Context flags
    enum class ContextFlag : int
    {
        None = 0x0,
    };

    DEFINE_ENUM_FLAG_OPERATORS(ContextFlag);

    // The context within which all commands execute.
    // Contains arguments via Execution::Args.
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

        // Applies changes based on the parsed args.
        void UpdateForArgs() {};

        // Gets context flags
        ContextFlag GetFlags() const
        {
            return m_flags;
        }

        // Set context flags
        void SetFlags(ContextFlag flags)
        {
            WI_SetAllFlags(m_flags, flags);
        }

        // Clear context flags
        void ClearFlags(ContextFlag flags)
        {
            WI_ClearAllFlags(m_flags, flags);
        }

        ////virtual void SetExecutionStage(Workflow::ExecutionStage stage);

        // Gets the executing command
        wsl::windows::wslc::Command* GetExecutingCommand() { return m_executingCommand; }

        // Sets the executing command
        void SetExecutingCommand(wsl::windows::wslc::Command* command) { m_executingCommand = command; }

    private:
        bool m_isTerminated = false;
        ContextFlag m_flags = ContextFlag::None;
        ////Workflow::ExecutionStage m_executionStage = Workflow::ExecutionStage::Initial;
        wsl::windows::wslc::Command* m_executingCommand = nullptr;
    };
}
