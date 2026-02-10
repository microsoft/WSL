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
    // Container Delete Command
    std::vector<Argument> ContainerDeleteCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::ContainerIdOpt),
            Argument::ForType(ArgType::All),
            Argument::ForType(ArgType::ForceD),
            Argument::ForType(ArgType::SessionId),
        };
    }

    std::wstring_view ContainerDeleteCommand::ShortDescription() const
    {
        return {L"Delete containers."};
    }

    std::wstring_view ContainerDeleteCommand::LongDescription() const
    {
        return {L"Deletes specified container(s). Use --force to delete "
            "running containers. Use --all to delete all the running containers. "};
    }

    void ContainerDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Delete subcommand executing..", stdout);
    }
}