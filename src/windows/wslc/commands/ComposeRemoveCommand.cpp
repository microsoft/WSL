/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeRemoveCommand.cpp

Abstract:

    Implements the Compose remove command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeRemoveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Project, true),
    };
}

std::wstring ComposeRemoveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeRemoveDesc();
}

std::wstring ComposeRemoveCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeRemoveLongDesc();
}

void ComposeRemoveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession << RemoveCompose;
}

} // namespace wsl::windows::wslc
