/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeDeleteCommand.cpp

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
std::vector<Argument> VolumeDeleteCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring VolumeDeleteCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeDeleteDesc();
}

std::wstring VolumeDeleteCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeDeleteLongDesc();
}

// clang-format off
void VolumeDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << DeleteVolumes;
}
// clang-format on
} // namespace wsl::windows::wslc
