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
    // Container Stop Command
    std::vector<Argument> ContainerStopCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::ContainerIdOpt),
            Argument::ForType(ArgType::All),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::SignalS),
            Argument::ForType(ArgType::Time),
        };
    }

    std::wstring_view ContainerStopCommand::ShortDescription() const
    {
        return {L"Sends a signal to stop running containers."};
    }

    std::wstring_view ContainerStopCommand::LongDescription() const
    {
        return {L"Sends a signal to stop running containers. Waits for a specified timout before issuing "
            "a SIGKILL. Stops all running containers if --all is used. Multiple space-separated container "
            "ids can be specified to stop the specified multiple containers. No containers are stopped if "
            "neither container-ids nor --all options are specified. "};
    }

    void ContainerStopCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Stop subcommand executing..", stdout);
    }
}