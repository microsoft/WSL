/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeCreateCommand.cpp

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
// Volume Create Command
std::vector<Argument> VolumeCreateCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true, std::nullopt, L"Name to give to the volume"), // Argument
        Argument::Create(ArgType::Label),
        Argument::Create(ArgType::Opt, std::nullopt, 10),
        Argument::Create(ArgType::SessionId),
        Argument::Create(ArgType::Size),
    };
}

std::wstring_view VolumeCreateCommand::ShortDescription() const
{
    return {L"Creates a new volume."};
}

std::wstring_view VolumeCreateCommand::LongDescription() const
{
    return {L"Creates a new volume."};
}

void VolumeCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Volume Create subcommand executing..", stdout);
}
} // namespace wsl::windows::wslc
