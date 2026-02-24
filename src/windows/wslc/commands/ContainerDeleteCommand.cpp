/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerDeleteCommand.cpp

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
// Container Delete Command
std::vector<Argument> ContainerDeleteCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Force),
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring ContainerDeleteCommand::ShortDescription() const
{
    return {L"Delete containers"};
}

std::wstring ContainerDeleteCommand::LongDescription() const
{
    return {L"Deletes containers."};
}

// clang-format off
void ContainerDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context 
        << CreateSession
        << DeleteContainers;
}
// clang-format on
} // namespace wsl::windows::wslc