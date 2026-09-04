/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeCreateCommand.cpp

Abstract:

    Implements the Compose create command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeCreateCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Path, true, std::nullopt, Localization::WSLCCLI_ComposePathArgDescription()),
    };
}

std::wstring ComposeCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeCreateDesc();
}

std::wstring ComposeCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeCreateLongDesc();
}

void ComposeCreateCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context << ResolveSession << CreateCompose;
}

} // namespace wsl::windows::wslc
