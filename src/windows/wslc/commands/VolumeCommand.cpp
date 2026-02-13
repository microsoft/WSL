/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "VolumeCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Volume Root Command
std::vector<std::unique_ptr<Command>> VolumeCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(5);
    commands.push_back(std::make_unique<VolumeCreateCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeDeleteCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeInspectCommand>(FullName()));
    commands.push_back(std::make_unique<VolumeListCommand>(FullName()));
    commands.push_back(std::make_unique<VolumePruneCommand>(FullName()));
    return commands;
}

std::vector<Argument> VolumeCommand::GetArguments() const
{
    return {};
}

std::wstring_view VolumeCommand::ShortDescription() const
{
    return {L"Volume command"};
}

std::wstring_view VolumeCommand::LongDescription() const
{
    return {L"Volume command"};
}

void VolumeCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Volume command executing..", stdout);
}
} // namespace wsl::windows::wslc
