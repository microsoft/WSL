/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagRunCommand.cpp

Abstract:

    Implementation of the diag run command.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "DiagCommand.h"
#include "DiagTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Diag Run Command
std::vector<Argument> DiagRunCommand::GetArguments() const
{
    return {
        // Adding the Verbose flag arg, overriding description and leaving other defaults alone.
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the run process."),
    };
}

std::wstring_view DiagRunCommand::ShortDescription() const
{
    return {L"Run a container."};
}

std::wstring_view DiagRunCommand::LongDescription() const
{
    return {L"Runs a specified container."};
}

void DiagRunCommand::ExecuteInternal(CLIExecutionContext& context) const
{
}
} // namespace wsl::windows::wslc