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
    // Container Inspect Command
    std::vector<Argument> ContainerInspectCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ContainerId, true, 10),
            Argument::Create(ArgType::SessionId),
        };
    }

    std::wstring_view ContainerInspectCommand::ShortDescription() const
    {
        return {L"Inspect containers."};
    }

    std::wstring_view ContainerInspectCommand::LongDescription() const
    {
        return {L"Outputs details about the container(s) specified using container ID(s) in JSON format."};
    }

    void ContainerInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Inspect subcommand executing..", stdout);
    }
}
