/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.cpp

Abstract:

    Implementation of container command related execution logic.

--*/
#include "Argument.h"
#include "ArgumentValidation.h"
#include "CLIExecutionContext.h"
#include "ContainerModel.h"
#include "ContainerService.h"
#include "ContainerTasks.h"
#include "PullImageCallback.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "TablePrinter.h"
#include <wil/result_macros.h>

using namespace wsl::shared;
using namespace wsl::shared::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {
void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    PullImageCallback callback;
    auto result = ContainerService::Create(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>(), &callback);
    PrintMessage(MultiByteToWide(result.Id));
}

void CreateSession(CLIExecutionContext& context)
{
    std::optional<SessionOptions> options = std::nullopt;
    context.Data.Add<Data::Session>(SessionService::CreateSession(options));
}

void DeleteContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    bool force = context.Args.Contains(ArgType::Force);
    for (const auto& id : containerIds)
    {
        ContainerService::Delete(session, WideToMultiByte(id), force);
    }
}

void ExecContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    auto result = ContainerService::Exec(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ContainerId>()), context.Data.Get<Data::ContainerOptions>());
}

void GetContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Containers>(ContainerService::List(session));
}

void InspectContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    std::vector<wsl::windows::common::docker_schema::InspectContainer> result;
    for (const auto& id : containerIds)
    {
        auto inspectData = ContainerService::Inspect(session, WideToMultiByte(id));
        result.push_back(inspectData);
    }

    auto json = ToJson(result);
    PrintMessage(MultiByteToWide(json));
}

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    WSLASignal signal = WSLASignalSIGKILL;
    if (context.Args.Contains(ArgType::Signal))
    {
        signal = validation::GetWSLASignalFromString(context.Args.Get<ArgType::Signal>());
    }

    for (const auto& id : containerIds)
    {
        ContainerService::Kill(session, WideToMultiByte(id), signal);
    }
}

void ListContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Containers));
    auto& containers = context.Data.Get<Data::Containers>();

    // Filter by running state if --all is not specified
    if (!context.Args.Contains(ArgType::All))
    {
        auto shouldRemove = [](const ContainerInformation& container) {
            return container.State != WSLA_CONTAINER_STATE::WslaContainerStateRunning;
        };
        containers.erase(std::remove_if(containers.begin(), containers.end(), shouldRemove), containers.end());
    }

    if (context.Args.Contains(ArgType::Quiet))
    {
        // Print only the container ids
        for (const auto& container : containers)
        {
            PrintMessage(MultiByteToWide(container.Id));
        }

        return;
    }

    FormatType format = FormatType::Table; // Default is table
    if (context.Args.Contains(ArgType::Format))
    {
        format = validation::GetFormatTypeFromString(context.Args.Get<ArgType::Format>());
    }

    switch (format)
    {
    case FormatType::Json:
    {
        auto json = ToJson(containers);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        utils::TablePrinter tablePrinter({L"ID", L"NAME", L"IMAGE", L"STATE"});
        for (const auto& container : containers)
        {
            tablePrinter.AddRow({
                MultiByteToWide(container.Id),
                MultiByteToWide(container.Name),
                MultiByteToWide(container.Image),
                ContainerService::ContainerStateToString(container.State),
            });
        }

        tablePrinter.Print();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    PullImageCallback callback;
    ContainerService::Run(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>(), &callback);
}

void SetContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerOptions options;

    if (context.Args.Contains(ArgType::Name))
    {
        options.Name = WideToMultiByte(context.Args.Get<ArgType::Name>());
    }

    if (context.Args.Contains(ArgType::TTY))
    {
        options.TTY = true;
    }

    if (context.Args.Contains(ArgType::Detach))
    {
        options.Detach = true;
    }

    if (context.Args.Contains(ArgType::Interactive))
    {
        options.Interactive = true;
    }

    if (context.Args.Contains(ArgType::Command))
    {
        options.Arguments.emplace_back(WideToMultiByte(context.Args.Get<ArgType::Command>()));
    }

    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        auto const& forwardArgs = context.Args.Get<ArgType::ForwardArgs>();
        options.Arguments.reserve(options.Arguments.size() + forwardArgs.size());
        for (const auto& arg : forwardArgs)
        {
            options.Arguments.emplace_back(WideToMultiByte(arg));
        }
    }

    context.Data.Add<Data::ContainerOptions>(std::move(options));
}

void StartContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    const auto& id = WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    ContainerService::Start(context.Data.Get<Data::Session>(), id);
}

void StopContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containersToStop = context.Args.GetAll<ArgType::ContainerId>();
    StopContainerOptions options;
    if (context.Args.Contains(ArgType::Signal))
    {
        options.Signal = validation::GetWSLASignalFromString(context.Args.Get<ArgType::Signal>());
    }

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = validation::GetIntegerFromString<LONGLONG>(context.Args.Get<ArgType::Time>());
    }

    for (const auto& id : containersToStop)
    {
        ContainerService::Stop(context.Data.Get<Data::Session>(), WideToMultiByte(id), options);
    }
}
} // namespace wsl::windows::wslc::task
