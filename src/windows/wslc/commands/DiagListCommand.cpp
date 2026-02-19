/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DiagListCommand.cpp

Abstract:

    Implementation of the diag list command.

--*/
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "DiagCommand.h"
#include "DiagTasks.h"
#include "Task.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Diag List Command
std::vector<Argument> DiagListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Verbose, std::nullopt, std::nullopt, L"Show detailed information about the listed containers."),
    };
}

std::wstring DiagListCommand::ShortDescription() const
{
    return {L"List containers."};
}

std::wstring DiagListCommand::LongDescription() const
{
    return {L"Lists specified container(s)."};
}

void DiagListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << task::ListContainers;
}
} // namespace wsl::windows::wslc
