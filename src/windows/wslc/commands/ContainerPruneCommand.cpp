/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerPruneCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "ContainerCommand.h"
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Container Prune Command
std::vector<Argument> ContainerPruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::PruneFilter, false, Limit::Unlimited),
        Argument::Create(ArgType::Force, std::nullopt, std::nullopt, Localization::WSLCCLI_PruneForceArgDescription()),
    };
}

std::wstring ContainerPruneCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerPruneDesc();
}

std::wstring ContainerPruneCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerPruneLongDesc();
}

void ContainerPruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context.Data.Add<Data::ConfirmWarning>(Localization::WSLCCLI_ContainerPruneConfirm());
    context.Data.Add<Data::ConfirmMessage>(Localization::WSLCCLI_PruneConfirmPrompt());

    context               //
        << ResolveSession //
        << PruneContainers;
}
} // namespace wsl::windows::wslc
