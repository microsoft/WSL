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
#include "ImageModel.h"
#include "ContainerService.h"
#include "SessionModel.h"
#include "SessionService.h"
#include "Task.h"
#include "ContainerTasks.h"
#include "PullImageCallback.h"
#include <optional>
#include <wil/result_macros.h>

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::services;
using namespace wsl::windows::wslc::models;
using namespace wsl::windows::common;
using wsl::windows::common::docker_schema::InspectContainer;

namespace wsl::windows::wslc::task {
void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    PullImageCallback callback;
    auto result = ContainerService::Create(
        context.Data.Get<Data::Session>(),
        string::WideToMultiByte(context.Args.Get<ArgType::ImageId>()),
        context.Data.Get<Data::ContainerOptions>(),
        &callback);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(result.Id));
}

void CreateSession(CLIExecutionContext& context)
{
    std::optional<SessionOptions> options = std::nullopt;
    if (context.Args.Contains(ArgType::SessionId))
    {
        // TODO: Add session ID to the session options to open the specified session.
    }

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
        ContainerService::Delete(session, string::WideToMultiByte(id), force);
    }
}

void ExecContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    auto result = ContainerService::Exec(
        context.Data.Get<Data::Session>(),
        string::WideToMultiByte(context.Args.Get<ArgType::ContainerId>()),
        context.Data.Get<Data::ContainerOptions>());
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
    std::vector<InspectContainer> result;
    for (const auto& id : containerIds)
    {
        auto inspectData = ContainerService::Inspect(session, string::WideToMultiByte(id));
        result.push_back(inspectData);
    }

    auto json = wsl::shared::ToJson(result);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(json));
}

void KillContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    auto containerIds = context.Args.GetAll<ArgType::ContainerId>();
    ULONG signal = WSLASignalSIGKILL;
    if (context.Args.Contains(ArgType::Signal))
    {
        signal = std::stoul(context.Args.Get<ArgType::Signal>());
    }

    for (const auto& id : containerIds)
    {
        ContainerService::Kill(session, string::WideToMultiByte(id), signal);
    }
}

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::ContainerOptions));
    PullImageCallback callback;
    ContainerService::Run(
        context.Data.Get<Data::Session>(),
        string::WideToMultiByte(context.Args.Get<ArgType::ImageId>()),
        context.Data.Get<Data::ContainerOptions>(),
        &callback);
}

void SetContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerOptions options;

    if (context.Args.Contains(ArgType::Name))
    {
        options.Name = string::WideToMultiByte(context.Args.Get<ArgType::Name>());
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
        options.Arguments.emplace_back(string::WideToMultiByte(context.Args.Get<ArgType::Command>()));
    }

    if (context.Args.Contains(ArgType::ForwardArgs))
    {
        auto const& forwardArgs = context.Args.Get<ArgType::ForwardArgs>();
        options.Arguments.reserve(options.Arguments.size() + forwardArgs.size());
        for (const auto& arg : forwardArgs)
        {
            options.Arguments.emplace_back(string::WideToMultiByte(arg));
        }
    }

    context.Data.Add<Data::ContainerOptions>(std::move(options));
}

void StartContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    const auto& id = string::WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
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
} // namespace wsl::windows::wslc::task
