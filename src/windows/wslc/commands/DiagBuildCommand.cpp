/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagBuildCommand.cpp

Abstract:

    Implementation of the diag build command.

--*/
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
// Diag Build Command
std::vector<Argument> DiagBuildCommand::GetArguments() const
{
    return {
        // Adding the Verbose flag arg, overriding description and leaving other defaults alone.
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the build process."),
    };
}

std::wstring_view DiagBuildCommand::ShortDescription() const
{
    return {L"Build a container."};
}

std::wstring_view DiagBuildCommand::LongDescription() const
{
    return {L"Builds a specified container."};
}

void DiagBuildCommand::ExecuteInternal(CLIExecutionContext& context) const
{
}
} // namespace wsl::windows::wslc