/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "RegistryCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Registry Root Command
std::vector<std::unique_ptr<Command>> RegistryCommand::GetCommands() const
{
    return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
        std::make_unique<RegistryLoginCommand>(FullName()),
        std::make_unique<RegistryLogoutCommand>(FullName()),
    });
}

std::vector<Argument> RegistryCommand::GetArguments() const
{
    return {};
}

std::wstring_view RegistryCommand::ShortDescription() const
{
    return {L"Registry command"};
}

std::wstring_view RegistryCommand::LongDescription() const
{
    return {L"Registry command"};
}

void RegistryCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Registry command executing..", stdout);
}
} // namespace wsl::windows::wslc
