// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc
{
    // Container Exec Command
    std::vector<Argument> ContainerExecCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ContainerId, true),
            Argument::Create(ArgType::ProcessArgs),
            Argument::Create(ArgType::Detach),
            Argument::Create(ArgType::Env, false, 10),
            Argument::Create(ArgType::EnvFile),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::SessionId),
            Argument::Create(ArgType::TTY),
            Argument::Create(ArgType::User),
        };
    }

    std::wstring_view ContainerExecCommand::ShortDescription() const
    {
        return {L"Execute a command in a container."};
    }

    std::wstring_view ContainerExecCommand::LongDescription() const
    {
        return {L"Executes a command in a container."};
    }

    void ContainerExecCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        if (context.Args.Contains(ArgType::ContainerId))
        {
            auto containerId = context.Args.Get<ArgType::ContainerId>();
            PrintMessage(L"Container Id: " + containerId);
        }

        if (context.Args.Contains(ArgType::ProcessArgs))
        {
            auto forwardedArgs = context.Args.Get<ArgType::ProcessArgs>();
            PrintMessage(L"Command with " + std::to_wstring(forwardedArgs.size()) + L" args:");
            std::wstring concatenatedArgs = std::accumulate(
                forwardedArgs.begin(),
                forwardedArgs.end(),
                std::wstring{},
                [](const std::wstring& a, const std::wstring& b) {
                    return a.empty() ? b : a + L" " + b;
                });

            PrintMessage(L"  Concatenated: " + concatenatedArgs);
        }

        if (context.Args.Contains(ArgType::Detach))
        {
            PrintMessage(L"  Detached mode");
        }

        if (context.Args.Contains(ArgType::Interactive))
        {
            PrintMessage(L"  Interactive mode");
        }

        if (context.Args.Contains(ArgType::TTY))
        {
            PrintMessage(L"  TTY allocated");
        }

        if (context.Args.Contains(ArgType::User))
        {
            auto user = context.Args.Get<ArgType::User>();
            PrintMessage(L"  User: " + user);
        }

        if (context.Args.Contains(ArgType::Env))
        {
            for (const auto& env : context.Args.GetAll<ArgType::Env>())
            {
                PrintMessage(L"  Env: " + env);
            }
        }

        if (context.Args.Contains(ArgType::EnvFile))
        {
            auto envFile = context.Args.Get<ArgType::EnvFile>();
            PrintMessage(L"  Env File: " + envFile);
        }

        // TODO: Implement actual container exec logic
        // This will involve:
        // 1. Resolving the container ID
        // 2. Setting up environment variables
        // 3. Configuring TTY/interactive mode if requested
        // 4. Executing the command in the container via WSLA APIs
        // 5. Handling detach mode if specified
    }
}
