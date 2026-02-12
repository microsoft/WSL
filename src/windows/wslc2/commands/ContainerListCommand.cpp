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
    // Container List Command
    std::vector<Argument> ContainerListCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ContainerId, L"Desc Override", false, 10),
            Argument::Create(ArgType::All),
            Argument::Create(ArgType::Format),
            Argument::Create(ArgType::Quiet),
            Argument::Create(ArgType::SessionId),
        };
    }

    std::wstring_view ContainerListCommand::ShortDescription() const
    {
        return {L"List containers."};
    }

    std::wstring_view ContainerListCommand::LongDescription() const
    {
        return {L"Lists specified container(s). Use --all to list all the running containers. "};
    }

    void ContainerListCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container List subcommand executing..", stdout);
    }
}