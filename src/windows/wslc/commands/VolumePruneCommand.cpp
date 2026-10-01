/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumePruneCommand.cpp

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
// Volume Prune Command
std::vector<Argument> VolumePruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All, {.Desc = Localization::WSLCCLI_VolumePruneAllArgDescription()}),
        Argument::Create(ArgType::Filter, {.Alias = NO_ALIAS, .Limit = Limit::Unlimited, .Desc = Localization::WSLCCLI_VolumePruneFilterArgDescription()}),
        Argument::Create(ArgType::Force, {.Desc = Localization::WSLCCLI_PruneForceArgDescription()}),
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
    context               //
        << ResolveSession //
        << PruneVolumes;
}
} // namespace wsl::windows::wslc
