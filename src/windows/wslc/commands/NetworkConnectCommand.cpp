/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkConnectCommand.cpp

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
// Network Connect Command
std::vector<Argument> NetworkConnectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::NetworkName, true),
        Argument::Create(ArgType::ContainerId, true),
    };
}

std::wstring NetworkConnectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkConnectDesc();
}

std::wstring NetworkConnectCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkConnectLongDesc();
}

void NetworkConnectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession //
            << ConnectNetwork;
}
} // namespace wsl::windows::wslc
