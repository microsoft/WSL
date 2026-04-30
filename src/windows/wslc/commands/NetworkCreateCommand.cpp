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
        Argument::Create(ArgType::NetworkName, true),
        Argument::Create(ArgType::Driver),
        Argument::Create(ArgType::Options, false, NO_LIMIT),
        Argument::Create(ArgType::Label, false, NO_LIMIT),
        Argument::Create(ArgType::Session),
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
    context << CreateSession //
            << CreateNetwork;
}
} // namespace wsl::windows::wslc
