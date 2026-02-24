/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerStopCommand.cpp

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
#include "SessionTasks.h"
#include "Task.h"

#include <wslservice.h>
#include <wslaservice.h>
#include <docker_schema.h>

using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::wslc::models::ContainerInformation;
using wsl::windows::wslc::services::ContainerService;
using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc {
// Container Stop Command
std::vector<Argument> ContainerStopCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGTERM)"),
        Argument::Create(ArgType::Time),
    };
}

std::wstring ContainerStopCommand::ShortDescription() const
{
    return {L"Stop containers"};
}

std::wstring ContainerStopCommand::LongDescription() const
{
    return {L"Stops containers."};
}

// clang-format off
void ContainerStopCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << StopContainers;
}
// clang-format on
} // namespace wsl::windows::wslc