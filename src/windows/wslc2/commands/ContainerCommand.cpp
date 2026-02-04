// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "ContainerCommand.h"
#include "TaskBase.h"
#include "TestTasks.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    // Container Root Command
    std::vector<std::unique_ptr<Command>> ContainerCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<ContainerRunCommand>(FullName()),
            std::make_unique<ContainerStartCommand>(FullName()),
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

    // Container Start Command
    std::vector<Argument> ContainerStartCommand::GetArguments() const
    {
        return {
            // ContainerId should be first by position, but we need to adjust the parser
            // for WSLC format of command <options> <positional> <args | positional2..>
            Argument::ForType(Args::Type::Attach),
            Argument::ForType(Args::Type::Interactive),
            Argument::ForType(Args::Type::ContainerId),
        };
    }

    std::wstring_view ContainerStartCommand::ShortDescription() const
    {
        return {L"Start command"};
    }

    std::wstring_view ContainerStartCommand::LongDescription() const
    {
        return {L"Start command for demonstration purposes."};
    }

    void ContainerStartCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Start subcommand executing..", stdout);
        PrintMessage(L"  ContainerId: " + std::wstring{context.Args.GetArg(Args::Type::ContainerId)});
        if (context.Args.Contains(Args::Type::Interactive))
        {
            PrintMessage(L"  Interactive mode");
        }

        if (context.Args.Contains(Args::Type::Attach))
        {
            PrintMessage(L"  Attach to stdout/stderr");
        }

        if (context.Args.Contains(Args::Type::SessionId))
        {
            PrintMessage(L"  Using SessionId: " + std::wstring{context.Args.GetArg(Args::Type::SessionId)});
        }

    }
    }
