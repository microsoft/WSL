/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeDeleteCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#pragma once
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
// Volume Delete Command
std::vector<Argument> VolumeDeleteCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, std::nullopt, 10, L"Names of the volumes to delete"), // Argument
        Argument::Create(ArgType::All, std::nullopt, std::nullopt, L"Delete all volumes"),
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring_view VolumeDeleteCommand::ShortDescription() const
{
    return {L"Deletes specified volume(s)."};
}

std::wstring_view VolumeDeleteCommand::LongDescription() const
{
    return {
        L"Deletes specified volume(s). One or more volumes can be specified. Volumes that are currently in use by either running "
        L"or stopped containers cannot be deleted. "};
}

void VolumeDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Volume Delete subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc
