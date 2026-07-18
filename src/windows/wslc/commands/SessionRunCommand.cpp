/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionRunCommand.cpp

Abstract:

    Implementation of the session run command.

--*/
#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Session Run Command
std::vector<Argument> SessionRunCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Command, true),
        Argument::Create(ArgType::ForwardArgs, std::nullopt, std::nullopt, Localization::WSLCCLI_SessionRunForwardArgsDescription()),
    };
}

std::wstring SessionRunCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SessionRunDesc();
}

std::wstring SessionRunCommand::LongDescription() const
{
    return Localization::WSLCCLI_SessionRunLongDesc();
}

void SessionRunCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession << RunInSession;
}
} // namespace wsl::windows::wslc
