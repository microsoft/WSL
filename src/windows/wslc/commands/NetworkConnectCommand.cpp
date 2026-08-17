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
        Argument::Create(ArgType::DriverOpt, false, Limit::Unlimited),
        Argument::Create(ArgType::IpAddress, false),
        Argument::Create(ArgType::Link, false, Limit::Unlimited),
        Argument::Create(ArgType::LinkLocalIp, false, Limit::Unlimited),
        Argument::Create(ArgType::NetworkAlias, false, Limit::Unlimited),
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
    context << ResolveSession                    //
            << SetNetworkEndpointOptionsFromArgs //
            << ConnectNetwork;
}
} // namespace wsl::windows::wslc
