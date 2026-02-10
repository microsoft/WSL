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
            Argument::ForType(ArgType::ContainerId),
            Argument::ForType(ArgType::ForwardArgsP),
            Argument::ForType(ArgType::Detach),
            Argument::ForType(ArgType::Env),
            Argument::ForType(ArgType::EnvFile),
            Argument::ForType(ArgType::Interactive),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::TTY),
            Argument::ForType(ArgType::User),
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
        PrintMessage(L"Container Exec subcommand executing..", stdout);
    }
}
