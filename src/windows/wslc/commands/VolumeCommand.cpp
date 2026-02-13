// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "VolumeCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc
{
    // Volume Root Command
    std::vector<std::unique_ptr<Command>> VolumeCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<VolumeCreateCommand>(FullName()),
            std::make_unique<VolumeDeleteCommand>(FullName()),
            std::make_unique<VolumeInspectCommand>(FullName()),
            std::make_unique<VolumeListCommand>(FullName()),
            std::make_unique<VolumePruneCommand>(FullName()),
        });
    }
 
    std::vector<Argument> VolumeCommand::GetArguments() const
    {
        return {};
    }

    std::wstring_view VolumeCommand::ShortDescription() const
    {
        return { L"Volume command" };
    }

    std::wstring_view VolumeCommand::LongDescription() const
    {
        return { L"Volume command" };
    }

    void VolumeCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Volume command executing..", stdout);
    }
}
