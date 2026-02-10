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
    // Image List Command
    std::vector<Argument> ImageListCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::Format),
            Argument::ForType(ArgType::Quiet),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::Verbose),
        };
    }

    std::wstring_view ImageListCommand::ShortDescription() const
    {
        return {L"Lists all the locally present images."};
    }

    std::wstring_view ImageListCommand::LongDescription() const
    {
        return {L"Lists all the locally present images. Supports either JSON or table formats."
            " When verbose output is requested, details like image ID, creation time, and size are output."};
    }

    void ImageListCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Image List subcommand executing..", stdout);
    }
}