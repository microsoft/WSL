// Copyright (C) Microsoft Corporation. All rights reserved.

#include "ContainerCommand.h"
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Container Cp Command
std::vector<Argument> ContainerCpCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Archive),
        Argument::Create(ArgType::Source, true, std::nullopt, Localization::WSLCCLI_CpSourceArgDescription()),
        Argument::Create(ArgType::Target, true, std::nullopt, Localization::WSLCCLI_CpTargetArgDescription()),
    };
}

std::wstring ContainerCpCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerCpDesc();
}

std::wstring ContainerCpCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerCpLongDesc();
}

void ContainerCpCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context               //
        << ResolveSession //
        << ContainerCp;
}
} // namespace wsl::windows::wslc
