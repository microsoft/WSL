/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SystemCommand.cpp

Abstract:

    Definition of System command tree.

--*/
#include "CLIExecutionContext.h"
#include "SystemCommand.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
std::vector<std::unique_ptr<Command>> SystemCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<SystemInfoCommand>(FullName()));
    commands.push_back(std::make_unique<SessionCommand>(FullName()));
    return commands;
}

std::vector<Argument> SystemCommand::GetArguments() const
{
    return {};
}

std::wstring SystemCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SystemCommandDesc();
}

std::wstring SystemCommand::LongDescription() const
{
    return Localization::WSLCCLI_SystemCommandLongDesc();
}

void SystemCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp(context.Terminal);
}

// System Info Command
std::vector<Argument> SystemInfoCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
    };
}

std::wstring SystemInfoCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SystemInfoDesc();
}

std::wstring SystemInfoCommand::LongDescription() const
{
    return Localization::WSLCCLI_SystemInfoLongDesc();
}

void SystemInfoCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ShowSystemInfo;
}
} // namespace wsl::windows::wslc
