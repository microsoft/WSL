/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeUpCommand.cpp

Abstract:

    Implements the Compose up command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeUpCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Path, true, std::nullopt, Localization::WSLCCLI_ComposePathArgDescription()),
    };
}

std::wstring ComposeUpCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeUpDesc();
}

std::wstring ComposeUpCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeUpLongDesc();
}

void ComposeUpCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context << ResolveSession << UpCompose;
}

} // namespace wsl::windows::wslc
