/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.cpp

Abstract:

    Implementation of container command related execution logic.

--*/
#include "Argument.h"
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

namespace wsl::windows::wslc::task {

namespace {
    void PopulateCommonContainerOptionsFromArgs(CLIExecutionContext& context, ContainerCreateOptions& options)
    {
        if (context.Args.Contains(ArgType::Name))
        {
            options.Name = string::WideToMultiByte(context.Args.Get<ArgType::Name>());
        }

        if (context.Args.Contains(ArgType::TTY))
        {
            options.TTY = true;
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
    }
} // anonymous namespace

void CreateSession(CLIExecutionContext& context)
{
    std::optional<SessionOptions> options = std::nullopt;
    if (context.Args.Contains(ArgType::SessionId))
    {
        // TODO: Add session ID to the session options to open the specified session.
    }

    context.Data.Add<Data::Session>(SessionService::CreateSession(options));
}

void GetContainers(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    auto& session = context.Data.Get<Data::Session>();
    context.Data.Add<Data::Containers>(ContainerService::List(session));
}

void StartContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ContainerId));
    const auto& id = string::WideToMultiByte(context.Args.Get<ArgType::ContainerId>());
    ContainerService::Start(context.Data.Get<Data::Session>(), id);
}

void SetCreateContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerCreateOptions options;
    PopulateCommonContainerOptionsFromArgs(context, options);
    context.Data.Add<Data::CreateContainerOptions>(std::move(options));
}

void SetRunContainerOptionsFromArgs(CLIExecutionContext& context)
{
    ContainerRunOptions options;
    PopulateCommonContainerOptionsFromArgs(context, options);
    if (context.Args.Contains(ArgType::Detach))
    {
        options.Detach = true;
    }

    context.Data.Add<Data::RunContainerOptions>(std::move(options));
}

void CreateContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::CreateContainerOptions));
    PullImageCallback callback;
    auto result = ContainerService::Create(
        context.Data.Get<Data::Session>(),
        string::WideToMultiByte(context.Args.Get<ArgType::ImageId>()),
        context.Data.Get<Data::CreateContainerOptions>(),
        &callback);
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(result.Id));
}

void RunContainer(CLIExecutionContext& context)
{
    WI_ASSERT(context.Data.Contains(Data::Session));
    WI_ASSERT(context.Args.Contains(ArgType::ImageId));
    WI_ASSERT(context.Data.Contains(Data::RunContainerOptions));
    PullImageCallback callback;
    ContainerService::Run(
        context.Data.Get<Data::Session>(),
        string::WideToMultiByte(context.Args.Get<ArgType::ImageId>()),
        context.Data.Get<Data::RunContainerOptions>(),
        &callback);
}
} // namespace wsl::windows::wslc::task
