// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "ContainerCommand.h"
#include "WorkflowBase.h"
#include "TestFlow.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    // Container Root Command
    std::vector<std::unique_ptr<Command>> ContainerCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<ContainerRunCommand>(FullName()),
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

    // Container Run Command
    std::vector<Argument> ContainerRunCommand::GetArguments() const
    {
        return {};
    }

    std::wstring_view ContainerRunCommand::ShortDescription() const
    {
        return { L"Run command" };
    }

    std::wstring_view ContainerRunCommand::LongDescription() const
    {
        return { L"Run command for demonstration purposes." };
    }

    void ContainerRunCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Run subcommand executing..", stdout);
    }
}
