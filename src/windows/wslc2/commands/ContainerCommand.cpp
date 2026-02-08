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
            Argument::ForType(ArgType::ContainerId),
            Argument::ForType(ArgType::ForwardArgs),
            Argument::ForType(ArgType::Attach),
            Argument::ForType(ArgType::Interactive),
            Argument::ForType(ArgType::SessionId),
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
        context << StoreSessionId;

        PrintMessage(L"Container Start subcommand executing..", stdout);
        ////PrintMessage(L"  ContainerId: " + std::wstring{context.Args.Get<ArgType::ContainerId>()});
        PrintMessage(L"  ContainerIds: ");

        for (const auto& id : context.Args.Get<ArgType::ContainerIds>())
        {
            PrintMessage(L"    Container Id: " + id);
        }


        if (context.Args.Contains(ArgType::Interactive))
        {
            PrintMessage(L"  Interactive mode");
        }

        if (context.Args.Contains(ArgType::Attach))
        {
            PrintMessage(L"  Attach to stdout/stderr");
        }

        if (context.Data.Contains(Data::SessionId))
        {
            PrintMessage(L"  Stored SessionId: " + context.Data.Get<Data::SessionId>());
        }

        auto keys = context.Data.GetKeys();
        auto dataCount = keys.size();

        PrintMessage(L"  Context contains " + std::to_wstring(dataCount) + L" data items:");
        for (const auto& key : keys)
        {
            PrintMessage(L"    - Data key: " + std::to_wstring(static_cast<int>(key)));
        }

        if (context.Args.Contains(ArgType::Port))
        {
            PrintMessage(L"  Ports: ");
            for (const auto& port : context.Args.GetAll<ArgType::Port>())
            {
                PrintMessage(L"    Port: " + port);
            }
        }

    }
}
