/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkDisconnectCommand.cpp

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
// Network Disconnect Command
std::vector<Argument> NetworkDisconnectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::NetworkName, true),
        Argument::Create(ArgType::ContainerId, true),
    };
}

std::wstring NetworkDisconnectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkDisconnectDesc();
}

std::wstring NetworkDisconnectCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkDisconnectLongDesc();
}

void NetworkDisconnectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession //
            << DisconnectNetwork;
}
} // namespace wsl::windows::wslc
