/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeRemoveCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "VolumeCommand.h"
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "VolumeTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Volume Delete Command
std::vector<Argument> VolumeRemoveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true, NO_LIMIT),
        Argument::Create(ArgType::Force, std::nullopt, std::nullopt, Localization::WSLCCLI_VolumeForceArgDescription()),
    };
}

std::wstring VolumeRemoveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeRemoveDesc();
}

std::wstring VolumeRemoveCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeRemoveLongDesc();
}

void VolumeRemoveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession //
            << DeleteVolumes;
}
} // namespace wsl::windows::wslc
