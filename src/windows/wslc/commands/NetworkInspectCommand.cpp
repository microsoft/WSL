/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkInspectCommand.cpp

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
// Network Inspect Command
std::vector<Argument> NetworkInspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::NetworkName, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring NetworkInspectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkInspectDesc();
}

std::wstring NetworkInspectCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkInspectLongDesc();
}

void NetworkInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << InspectNetworks;
}
} // namespace wsl::windows::wslc
