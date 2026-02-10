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
    // Container Kill Command
    std::vector<Argument> ContainerKillCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::ContainerIdOpt),
            Argument::ForType(ArgType::All),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::SignalK),
        };
    }

    std::wstring_view ContainerKillCommand::ShortDescription() const
    {
        return {L"Immediately kill containers."};
    }

    std::wstring_view ContainerKillCommand::LongDescription() const
    {
        return {L"Sends SIGKILL (default option) to running containers to immediately kill the containers. "};
    }

    void ContainerKillCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Kill subcommand executing..", stdout);
    }
}