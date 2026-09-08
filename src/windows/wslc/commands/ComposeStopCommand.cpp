/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeStopCommand.cpp

Abstract:

    Implements the Compose stop command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeStopCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Project, true),
        Argument::Create(ArgType::Time),
    };
}

std::wstring ComposeStopCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeStopDesc();
}

std::wstring ComposeStopCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeStopLongDesc();
}

void ComposeStopCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession << StopCompose;
}

} // namespace wsl::windows::wslc
