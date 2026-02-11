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
    // Volume Create Command
    std::vector<Argument> VolumeCreateCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::VolumeNameC),
            Argument::ForType(ArgType::Label),
            Argument::ForType(ArgType::Opt),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::Size),
        };
    }

    std::wstring_view VolumeCreateCommand::ShortDescription() const
    {
        return {L"Creates a new volume."};
    }

    std::wstring_view VolumeCreateCommand::LongDescription() const
    {
        return {L"Creates a new volume."};
    }

    void VolumeCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Volume Create subcommand executing..", stdout);
    }
}
