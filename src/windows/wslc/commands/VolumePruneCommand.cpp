/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumePruneCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "VolumeCommand.h"
#include "CLIExecutionContext.h"
#include "CommonTasks.h"
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
        Argument::Create(ArgType::All, std::nullopt, std::nullopt, Localization::WSLCCLI_VolumePruneAllArgDescription()),
        Argument::Create(ArgType::PruneFilter, false, Limit::Unlimited, Localization::WSLCCLI_VolumePruneFilterArgDescription()),
        Argument::Create(ArgType::Force, std::nullopt, std::nullopt, Localization::WSLCCLI_PruneForceArgDescription()),
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
    context.Data.Add<Data::ConfirmWarning>(
        context.Args.GetValue<ArgType::All>() ? Localization::WSLCCLI_VolumePruneAllConfirm() : Localization::WSLCCLI_VolumePruneConfirm());
    context.Data.Add<Data::ConfirmMessage>(Localization::WSLCCLI_PruneConfirmPrompt());

    context               //
        << ResolveSession //
        << ConfirmAction  //
        << PruneVolumes;
}
} // namespace wsl::windows::wslc
