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
    // Container Start Command
    std::vector<Argument> ContainerStartCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ContainerId, true),
            Argument::Create(ArgType::Attach),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::SessionId),
        };
    }

    std::wstring_view ContainerStartCommand::ShortDescription() const
    {
        return {L"Start a container."};
    }

    std::wstring_view ContainerStartCommand::LongDescription() const
    {
        return {L"Start a container. Provides options to attach to the "
            "container's stdout and stderr streams and could be interactive "
            "to keep stdin open. "};
    }

    void ContainerStartCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Start subcommand executing..", stdout);
    }
}