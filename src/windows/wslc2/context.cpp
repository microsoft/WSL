// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "argument.h"
#include "command.h"
#include "context.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc;

namespace wsl::windows::wslc::execution
{
    void CLIExecutionContext::UpdateForArgs()
    {
        // Currently no-op.
    }

    void CLIExecutionContext::Terminate(HRESULT hr, std::string_view file, size_t line)
    {
        ////Logging::Telemetry().LogCommandTermination(hr, file, line);
        if (!m_isTerminated)
        {
            SetTerminationHR(hr);
        }
    }

    void CLIExecutionContext::SetTerminationHR(HRESULT hr)
    {
        m_terminationHR = hr;
        m_isTerminated = true;
    }

    void CLIExecutionContext::SetExecutionStage(ExecutionStage stage)
    {
        if (m_executionStage == stage)
        {
            return;
        }
        else if (m_executionStage > stage)
        {
            // Programmer error.
            THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), "Reporting ExecutionStage to an earlier Stage.");
        }

        m_executionStage = stage;
    }
}
