/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeStartCommand.cpp

Abstract:

    Implements the Compose start command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeStartCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Path, true, std::nullopt, Localization::WSLCCLI_ComposePathArgDescription()),
    };
}

std::wstring ComposeStartCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeStartDesc();
}

std::wstring ComposeStartCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeStartLongDesc();
}

void ComposeStartCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context << ResolveSession << StartCompose;
}

} // namespace wsl::windows::wslc
