/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SystemInfoCommand.cpp

Abstract:

    Implementation of the system info command.

--*/
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "SystemCommand.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
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
