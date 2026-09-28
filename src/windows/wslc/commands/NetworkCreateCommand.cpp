/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkCreateCommand.cpp

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
// Network Create Command
std::vector<Argument> NetworkCreateCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::NetworkName, {.Required = true}),
        Argument::Create(ArgType::Driver, {.Desc = Localization::WSLCCLI_NetworkDriverOptionDescription()}),
        Argument::Create(ArgType::Options, {.Limit = Limit::Unlimited}),
        Argument::Create(ArgType::Label, {.Limit = Limit::Unlimited, .Desc = Localization::WSLCCLI_NetworkLabelArgDescription()}),
        Argument::Create(ArgType::Gateway),
        Argument::Create(ArgType::Internal),
        Argument::Create(ArgType::IpRange),
        Argument::Create(ArgType::Subnet),
    };
}

std::wstring NetworkCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkCreateDesc();
}

std::wstring NetworkCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkCreateLongDesc();
}

void NetworkCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession //
            << CreateNetwork;
}
} // namespace wsl::windows::wslc
