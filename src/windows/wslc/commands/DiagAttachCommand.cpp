/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagAttachCommand.cpp

Abstract:

    Implementation of the diag attach command.

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
// Diag Attach Command
std::vector<Argument> DiagAttachCommand::GetArguments() const
{
    return {
        // Adding the Verbose flag arg, overriding description and leaving other defaults alone.
        Argument::Create(
            ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the attached containers."),
    };
}

std::wstring_view DiagAttachCommand::ShortDescription() const
{
    return {L"Attach to a container."};
}

std::wstring_view DiagAttachCommand::LongDescription() const
{
    return {L"Attaches to a specified container."};
}

void DiagAttachCommand::ExecuteInternal(CLIExecutionContext& context) const
{
}
} // namespace wsl::windows::wslc
