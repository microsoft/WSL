/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkRemoveCommand.cpp

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
// Network Remove Command
std::vector<Argument> NetworkRemoveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::NetworkName, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring NetworkRemoveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkRemoveDesc();
}

std::wstring NetworkRemoveCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkRemoveLongDesc();
}

void NetworkRemoveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << DeleteNetworks;
}
} // namespace wsl::windows::wslc
