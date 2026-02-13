/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagPullCommand.cpp

Abstract:

    Implementation of the diag pull command.

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
// Diag Pull Command
std::vector<Argument> DiagPullCommand::GetArguments() const
{
    return {
        // Adding the Verbose flag arg, overriding description and leaving other defaults alone.
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the pull process."),
    };
}

std::wstring_view DiagPullCommand::ShortDescription() const
{
    return {L"Pull a container image."};
}

std::wstring_view DiagPullCommand::LongDescription() const
{
    return {L"Pulls a specified container image."};
}

void DiagPullCommand::ExecuteInternal(CLIExecutionContext& context) const
{
}
} // namespace wsl::windows::wslc