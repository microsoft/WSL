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
#include "SessionModel.h"
#include "SessionService.h"
#include "TableOutput.h"
#include <wil/result_macros.h>
#include <wslc_schema.h>

using namespace wsl::shared;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::wslc::services;

namespace wsl::windows::wslc::task {
void AttachContainer::operator()(CLIExecutionContext& context) const
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    context.ExitCode = ContainerService::Attach(context.Data.Get<Data::Session>(), WideToMultiByte(m_containerId));
}

void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    auto result = ContainerService::Create(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>());
    PrintMessage(MultiByteToWide(result.Id));
}

void ExecContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Exec(
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
    std::vector<wsl::windows::common::wslc_schema::InspectContainer> result;
    for (const auto& id : containerIds)
    {
        auto inspectData = ContainerService::Inspect(session, WideToMultiByte(id));
        result.push_back(inspectData);
    }

    auto json = ToJson(result, c_jsonPrettyPrintIndent);
    PrintMessage(MultiByteToWide(json));
}

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    WSLCSignal signal = WSLCSignalSIGKILL;
    if (context.Args.Contains(ArgType::Signal))
    {
        signal = validation::GetWSLCSignalFromString(context.Args.Get<ArgType::Signal>());
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
            return container.State != WSLCContainerState::WslcContainerStateRunning;
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
        auto json = ToJson(containers, c_jsonPrettyPrintIndent);
        PrintMessage(MultiByteToWide(json));
        break;
    }
    case FormatType::Table:
    {
        using Config = wsl::windows::wslc::ColumnWidthConfig;
        bool trunc = !context.Args.Contains(ArgType::NoTrunc);

        // Create table with or without column limits based on --no-trunc flag
        auto table =
            trunc ? wsl::windows::wslc::TableOutput<6>(
                        {{{L"CONTAINER ID", {Config::NoLimit, 12, false}},
                          {L"NAME", {Config::NoLimit, 20, true}},
                          {L"IMAGE", {Config::NoLimit, 20, false}},
                          {L"CREATED", {Config::NoLimit, Config::NoLimit, false}},
                          {L"STATUS", {Config::NoLimit, Config::NoLimit, false}},
                          {L"PORTS", {Config::NoLimit, Config::NoLimit, false}}}})
                  : wsl::windows::wslc::TableOutput<6>({L"CONTAINER ID", L"NAME", L"IMAGE", L"CREATED", L"STATUS", L"PORTS"});

        // Add each container as a row
        for (const auto& container : containers)
        {
            table.OutputLine({
                MultiByteToWide(trunc ? TruncateId(container.Id) : container.Id),
                MultiByteToWide(container.Name),
                MultiByteToWide(container.Image),
                ContainerService::FormatRelativeTime(container.CreatedAt),
                ContainerService::ContainerStateToString(container.State, container.StateChangedAt),
                ContainerService::FormatPorts(container.State, container.Ports),
            });
        }

        table.Complete();
        break;
    }
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

void RemoveContainers(CLIExecutionContext& context)
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

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    context.ExitCode = ContainerService::Run(
        context.Data.Get<Data::Session>(), WideToMultiByte(context.Args.Get<ArgType::ImageId>()), context.Data.Get<Data::ContainerOptions>());
}

void SetContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerOptions options;

    if (context.Args.Contains(ArgType::CIDFile))
    {
        options.CidFile = context.Args.Get<ArgType::CIDFile>();
    }

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

    if (context.Args.Contains(ArgType::Publish))
    {
        auto ports = context.Args.GetAll<ArgType::Publish>();
        options.Ports.reserve(options.Ports.size() + ports.size());
        for (const auto& port : ports)
        {
            options.Ports.emplace_back(WideToMultiByte(port));
        }
    }

    if (context.Args.Contains(ArgType::Volume))
    {
        auto volumes = context.Args.GetAll<ArgType::Volume>();
        options.Volumes.reserve(options.Volumes.size() + volumes.size());
        for (const auto& volume : volumes)
        {
            options.Volumes.emplace_back(volume);
        }
    }

    if (context.Args.Contains(ArgType::Remove))
    {
        options.Remove = true;
    }

    if (context.Args.Contains(ArgType::Command))
    {
        options.Arguments.emplace_back(WideToMultiByte(context.Args.Get<ArgType::Command>()));
    }

    if (context.Args.Contains(ArgType::EnvFile))
    {
        auto const& envFiles = context.Args.GetAll<ArgType::EnvFile>();
        for (const auto& envFile : envFiles)
        {
            auto parsedEnvVars = EnvironmentVariable::ParseFile(envFile);
            for (const auto& envVar : parsedEnvVars)
            {
                options.EnvironmentVariables.push_back(wsl::shared::string::WideToMultiByte(envVar));
            }
        }
    }

    if (context.Args.Contains(ArgType::Env))
    {
        auto const& envArgs = context.Args.GetAll<ArgType::Env>();
        for (const auto& arg : envArgs)
        {
            auto envVar = EnvironmentVariable::Parse(arg);
            if (envVar)
            {
                options.EnvironmentVariables.push_back(wsl::shared::string::WideToMultiByte(*envVar));
            }
        }
    }

    if (context.Args.Contains(ArgType::Entrypoint))
    {
        options.Entrypoint.push_back(WideToMultiByte(context.Args.Get<ArgType::Entrypoint>()));
    }

    if (context.Args.Contains(ArgType::User))
    {
        options.User = WideToMultiByte(context.Args.Get<ArgType::User>());
    }

    if (context.Args.Contains(ArgType::TMPFS))
    {
        auto tmpfs = context.Args.GetAll<ArgType::TMPFS>();
        options.Tmpfs.reserve(options.Tmpfs.size() + tmpfs.size());
        for (const auto& value : tmpfs)
        {
            options.Tmpfs.emplace_back(WideToMultiByte(value));
        }
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

    if (context.Args.Contains(ArgType::WorkDir))
    {
        options.WorkingDirectory = WideToMultiByte(context.Args.Get<ArgType::WorkDir>());
    }

    context.Data.Add<Data::ContainerOptions>(std::move(options));
}

void StartContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    const auto& id = WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    context.ExitCode = ContainerService::Start(context.Data.Get<Data::Session>(), id, context.Args.Contains(ArgType::Attach));
}

void StopContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containersToStop = context.Args.GetAll<ArgType::ContainerId>();
    StopContainerOptions options;
    if (context.Args.Contains(ArgType::Signal))
    {
        options.Signal = validation::GetWSLCSignalFromString(context.Args.Get<ArgType::Signal>());
    }

    if (context.Args.Contains(ArgType::Time))
    {
        options.Timeout = validation::GetIntegerFromString<LONG>(context.Args.Get<ArgType::Time>());
    }

    for (const auto& id : containersToStop)
    {
        ContainerService::Stop(context.Data.Get<Data::Session>(), WideToMultiByte(id), options);
    }
}

void ViewContainerLogs(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerId = context.Args.Get<ArgType::ContainerId>();
    bool follow = context.Args.Contains(ArgType::Follow);
    ContainerService::Logs(session, WideToMultiByte(containerId), follow);
}
} // namespace wsl::windows::wslc::task
