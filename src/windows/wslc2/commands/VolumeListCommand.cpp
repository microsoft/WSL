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
    // Volume List Command
    std::vector<Argument> VolumeListCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::Format),
            Argument::ForType(ArgType::Quiet),
            Argument::ForType(ArgType::SessionId),
        };
    }

    std::wstring_view VolumeListCommand::ShortDescription() const
    {
        return {L"Lists volumes."};
    }

    std::wstring_view VolumeListCommand::LongDescription() const
    {
        return {L"Lists volumes."};
    }

    void VolumeListCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Volume List subcommand executing..", stdout);
    }
}
