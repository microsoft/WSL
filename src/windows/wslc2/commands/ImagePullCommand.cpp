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
    // Image Pull Command
    std::vector<Argument> ImagePullCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ImageId, true),        // Argument
            Argument::Create(ArgType::Progress),
            Argument::Create(ArgType::Scheme),
            Argument::Create(ArgType::SessionId),
        };
    }

    std::wstring_view ImagePullCommand::ShortDescription() const
    {
        return {L"Pulls an image from a registry. "};
    }

    std::wstring_view ImagePullCommand::LongDescription() const
    {
        return {L"Pulls an image from a registry. "};
    }

    void ImagePullCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Image Pull subcommand executing..", stdout);
    }
}