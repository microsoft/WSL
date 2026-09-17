/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeListCommand.cpp

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
// Volume List Command
std::vector<Argument> VolumeListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Filter, {.Limit = Limit::Unlimited}),
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::Quiet, {.Desc = Localization::WSLCCLI_VolumeListQuietArgDesc()}),
    };
}

std::wstring VolumeListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeListDesc();
}

std::wstring VolumeListCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeListLongDesc();
}

void VolumeListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession //
            << GetVolumes     //
            << ListVolumes;
}
} // namespace wsl::windows::wslc
