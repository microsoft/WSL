/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumePruneCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "VolumeCommand.h"
#include "CLIExecutionContext.h"
#include "VolumeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Volume Prune Command
std::vector<Argument> VolumePruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All, std::nullopt, std::nullopt, Localization::WSLCCLI_VolumePruneAllArgDescription()),
        Argument::Create(ArgType::Session),
    };
}

std::wstring VolumePruneCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumePruneDesc();
}

std::wstring VolumePruneCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumePruneLongDesc();
}

void VolumePruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << PruneVolumes;
}
} // namespace wsl::windows::wslc
