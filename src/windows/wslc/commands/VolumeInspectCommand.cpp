/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeInspectCommand.cpp

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
// Volume Inspect Command
std::vector<Argument> VolumeInspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true, 10, L"Names of the volumes to inspect"), // Argument
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring_view VolumeInspectCommand::ShortDescription() const
{
    return {L"Inspects a volume."};
}

std::wstring_view VolumeInspectCommand::LongDescription() const
{
    return {L"Inspects a volume."};
}

void VolumeInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Volume Inspect subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc
