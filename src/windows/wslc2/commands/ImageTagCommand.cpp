// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ImageCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc
{
    // Image Tag Command
    std::vector<Argument> ImageTagCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::Source),
            Argument::Create(ArgType::Target),
            Argument::Create(ArgType::SessionId)
        };
    }

    std::wstring_view ImageTagCommand::ShortDescription() const
    {
        return {L"Tags an image with a new name. "};
    }

    std::wstring_view ImageTagCommand::LongDescription() const
    {
        return {L"Tags an image with a new name. "};
    }

    void ImageTagCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Image Tag subcommand executing..");
    }
}
