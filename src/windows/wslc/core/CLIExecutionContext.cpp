// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Argument.h"
#include "Command.h"
#include "CLIExecutionContext.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc;

namespace wsl::windows::wslc::execution
{
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
}
