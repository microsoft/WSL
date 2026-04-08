/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeCreateCommand.cpp

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
// Volume Create Command
std::vector<Argument> VolumeCreateCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::VolumeName, true),
        Argument::Create(ArgType::VolumeDriver),
        Argument::Create(ArgType::Session),
    };
}

std::wstring VolumeCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeCreateDesc();
}

std::wstring VolumeCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeCreateLongDesc();
}

// clang-format off
void VolumeCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << CreateVolume;
}
// clang-format on
} // namespace wsl::windows::wslc
