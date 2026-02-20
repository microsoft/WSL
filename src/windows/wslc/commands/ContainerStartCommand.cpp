/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStartCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "precomp.h"
#include "ContainerModel.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
#include "TablePrinter.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerTasks.h"
#include "Task.h"

using wsl::windows::common::string::WideToMultiByte;
using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::wslc::models::ContainerInformation;
using wsl::windows::wslc::services::ContainerService;
using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container Start Command
std::vector<Argument> ContainerStartCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Attach),      // NYI
        Argument::Create(ArgType::Interactive), // NYI
        Argument::Create(ArgType::SessionId),   // NYI
    };
}

std::wstring ContainerStartCommand::ShortDescription() const
{
    return {L"Start a container."};
}

std::wstring ContainerStartCommand::LongDescription() const
{
    return {
        L"Starts a container. Provides options to attach to the container's stdout and stderr streams and could be interactive "
        L"to keep stdin open."};
}

void ContainerStartCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession << StartContainer;
}
} // namespace wsl::windows::wslc