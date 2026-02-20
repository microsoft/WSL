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
        Argument::Create(ArgType::ContainerId, std::nullopt, -1),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::SessionId),
        Argument::Create(ArgType::Signal, std::nullopt, std::nullopt, L"Signal to send (default: SIGTERM)"),
        Argument::Create(ArgType::Time),
    };
}

std::wstring ContainerStopCommand::ShortDescription() const
{
    return {L"Stop a container."};
}

std::wstring ContainerStopCommand::LongDescription() const
{
    return {
        L"Stops a container. By default, the container is stopped in the background; use --detach to stop in the foreground."};
}

void ContainerStopCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession;

    auto containersToStop = context.Args.GetAll<ArgType::ContainerId>();
    if (context.Args.Contains(ArgType::All))
    {
        // All overwrites any specified container IDs.
        containersToStop.clear();
        context << GetContainers;
        const auto& allContainers = context.Data.Get<Data::Containers>();
        for (const auto& container : allContainers)
        {
            if (container.State == WSLA_CONTAINER_STATE::WslaContainerStateRunning)
            {
                containersToStop.emplace_back(string::MultiByteToWide(container.Name));
            }
        }
    }

    StopContainerOptions options;
    if (context.Args.Contains(ArgType::Signal))
    {
        options.Signal = std::stoul(context.Args.Get<ArgType::Signal>());
    }

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = std::stoul(context.Args.Get<ArgType::Time>());
    }

    for (const auto& id : containersToStop)
    {
        ContainerService::Stop(context.Data.Get<Data::Session>(), string::WideToMultiByte(id), options);
    }
}
} // namespace wsl::windows::wslc