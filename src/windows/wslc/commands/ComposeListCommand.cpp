// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeCommand.h"
#include "ComposeTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {

std::vector<Argument> ComposeListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::Quiet),
    };
}

std::wstring ComposeListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ComposeListDesc();
}

std::wstring ComposeListCommand::LongDescription() const
{
    return Localization::WSLCCLI_ComposeListLongDesc();
}

// clang-format off
void ComposeListCommand::ExecuteInternal(CLIExecutionContext& Context) const
{
    Context
        << ResolveSession
        << GetComposeProjects
        << ListComposeProjects;
}
// clang-format on

} // namespace wsl::windows::wslc
