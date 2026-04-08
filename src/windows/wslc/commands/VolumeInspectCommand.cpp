/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeInspectCommand.cpp

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
// Volume Inspect Command
std::vector<Argument> VolumeInspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring VolumeInspectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeInspectDesc();
}

std::wstring VolumeInspectCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeInspectLongDesc();
}

// clang-format off
void VolumeInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << InspectVolumes;
}
// clang-format on
} // namespace wsl::windows::wslc
