/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeAttachCommand.cpp

Abstract:

    Implements the Compose attach command.

--*/

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeAttachCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Project, true),
    };
}

std::wstring ComposeAttachCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeAttachDesc();
}

std::wstring ComposeAttachCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeAttachLongDesc();
}

void ComposeAttachCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << ResolveSession << AttachCompose;
}

} // namespace wsl::windows::wslc
