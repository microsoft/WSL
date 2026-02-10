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
    // Container Root Command
    std::vector<std::unique_ptr<Command>> ContainerCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<ContainerCreateCommand>(FullName()),
            std::make_unique<ContainerKillCommand>(FullName()),
            std::make_unique<ContainerRunCommand>(FullName()),
            std::make_unique<ContainerStartCommand>(FullName()),
            std::make_unique<ContainerStopCommand>(FullName()),
        });
    }
 
    std::vector<Argument> ContainerCommand::GetArguments() const
    {
        return {};
    }

    std::wstring_view ContainerCommand::ShortDescription() const
    {
        return { L"Container command" };
    }

    std::wstring_view ContainerCommand::LongDescription() const
    {
        return { L"Container command for demonstration purposes." };
    }

    void ContainerCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container base command executing..", stdout);
    }
}
