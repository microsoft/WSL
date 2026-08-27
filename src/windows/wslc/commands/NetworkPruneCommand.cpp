/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkPruneCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "NetworkCommand.h"
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "NetworkTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Network Prune Command
std::vector<Argument> NetworkPruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::PruneFilter, false, Limit::Unlimited),
        Argument::Create(ArgType::Force, std::nullopt, std::nullopt, Localization::WSLCCLI_PruneForceArgDescription()),
    };
}

std::wstring NetworkPruneCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkPruneDesc();
}

std::wstring NetworkPruneCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkPruneLongDesc();
}

void NetworkPruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context.Data.Add<Data::ConfirmWarning>(Localization::WSLCCLI_NetworkPruneConfirm());
    context.Data.Add<Data::ConfirmMessage>(Localization::WSLCCLI_PruneConfirmPrompt());

    context               //
        << ResolveSession //
        << PruneNetworks;
}
} // namespace wsl::windows::wslc
