/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerKillCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "precomp.h"
#include "ContainerModel.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
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
// Container Kill Command
std::vector<Argument> ContainerKillCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGKILL)"),
    };
}

std::wstring ContainerKillCommand::ShortDescription() const
{
    return {L"Kill containers"};
}

std::wstring ContainerKillCommand::LongDescription() const
{
    return {L"Kills containers."};
}

// clang-format off
void ContainerKillCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << KillContainers;
}
// clang-format on
} // namespace wsl::windows::wslc