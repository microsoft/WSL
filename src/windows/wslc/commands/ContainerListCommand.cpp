/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerListCommand.cpp

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

using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::wslc::models::ContainerInformation;
using wsl::windows::wslc::services::ContainerService;
using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container List Command
std::vector<Argument> ContainerListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, std::nullopt, 25, L"Include only the container names specified."),
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::Quiet),
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring ContainerListCommand::ShortDescription() const
{
    return {L"List containers."};
}

std::wstring ContainerListCommand::LongDescription() const
{
    return {L"Lists specified container(s). By default, only running containers are shown; use --all to include all containers."};
}

void ContainerListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession << GetContainers;

    auto& containers = context.Data.Get<Data::Containers>();

    // Filter by running state if --all is not specified
    if (!context.Args.Contains(ArgType::All))
    {
        auto shouldRemove = [](const ContainerInformation& container) {
            return container.State != WSLA_CONTAINER_STATE::WslaContainerStateRunning;
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    // Filter by name if provided
    if (context.Args.Contains(ArgType::ContainerId))
    {
        auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
        auto shouldRemove = [&containerIds](const ContainerInformation& container) {
            auto wideContainerName = string::MultiByteToWide(container.Name);
            return std::find(containerIds.begin(), containerIds.end(), wideContainerName) == containerIds.end();
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    if (context.Args.Contains(ArgType::Quiet))
    {
        // Print only the container ids
        for (const auto& container : containers)
        {
            PrintMessage(string::MultiByteToWide(container.Id));
        }
    }
    // TODO: When we have more arguments that have CLI-specific validation, centralize the validation logic
    // so they are validated prior to reaching command execution. We should check the format types during
    // ArgumentValidation in the command and error if the user put in an invalid format type, then the
    // command can safely assume the format type is valid here and doesn't need to check for it again.
    else if (context.Args.Contains(ArgType::Format) && (string::IsEqual(context.Args.Get<ArgType::Format>(), L"json", true)))
    {
        auto json = ToJson(containers);
        PrintMessage(string::MultiByteToWide(json));
    }
    else
    {
        utils::TablePrinter tablePrinter({L"ID", L"NAME", L"IMAGE", L"STATE"});
        for (const auto& container : containers)
        {
            tablePrinter.AddRow({
                string::MultiByteToWide(container.Id),
                string::MultiByteToWide(container.Name),
                string::MultiByteToWide(container.Image),
                ContainerService::ContainerStateToString(container.State),
            });
        }

        tablePrinter.Print();
    }
}
} // namespace wsl::windows::wslc