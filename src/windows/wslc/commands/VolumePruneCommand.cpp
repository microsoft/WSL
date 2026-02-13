/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumePruneCommand.cpp

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
// Volume Prune Command
std::vector<Argument> VolumePruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring_view VolumePruneCommand::ShortDescription() const
{
    return {L"Prunes unused volumes."};
}

std::wstring_view VolumePruneCommand::LongDescription() const
{
    return {L"Removes all dangling volumes reclaim disk space."};
}

void VolumePruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Volume Prune subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc
