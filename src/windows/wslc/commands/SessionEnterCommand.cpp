/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionEnterCommand.cpp

Abstract:

    Implementation of the session enter command.

--*/

#include "CLIExecutionContext.h"
#include "SessionCommand.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> SessionEnterCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::StoragePath, true),
        Argument::Create(ArgType::Name, std::nullopt, std::nullopt, Localization::WSLCCLI_SessionEnterNameArgDescription()),
    };
}

std::wstring SessionEnterCommand::ShortDescription() const
{
    return Localization::WSLCCLI_SessionEnterDesc();
}

std::wstring SessionEnterCommand::LongDescription() const
{
    return Localization::WSLCCLI_SessionEnterLongDesc();
}

void SessionEnterCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << EnterSession;
}
} // namespace wsl::windows::wslc
