/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerExportCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "ContainerCommand.h"
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Container Export Command
std::vector<Argument> ContainerExportCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Output, std::nullopt, std::nullopt, Localization::WSLCCLI_ContainerExportOutputArgDescription()),
    };
}

std::wstring ContainerExportCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerExportDesc();
}

std::wstring ContainerExportCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerExportLongDesc();
}

void ContainerExportCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context               //
        << ResolveSession //
        << ExportContainer;
}
} // namespace wsl::windows::wslc
