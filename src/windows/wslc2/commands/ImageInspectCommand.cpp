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
    // Image Inspect Command
    std::vector<Argument> ImageInspectCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ImageIdReq),
            Argument::Create(ArgType::SessionId),
        };
    }

    std::wstring_view ImageInspectCommand::ShortDescription() const
    {
        return {L"Outputs detailed information about image(s) in JSON format. "};
    }

    std::wstring_view ImageInspectCommand::LongDescription() const
    {
        return {L"Outputs detailed information about image(s) in JSON format. "};
    }

    void ImageInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Image Inspect subcommand executing..", stdout);
    }
}
