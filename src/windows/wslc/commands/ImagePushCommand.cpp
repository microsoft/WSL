/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImagePushCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
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

namespace wsl::windows::wslc {
// Image Push Command
std::vector<Argument> ImagePushCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true), // Argument
        Argument::Create(ArgType::Progress),
        Argument::Create(ArgType::Scheme),
        Argument::Create(ArgType::SessionId),
    };
}

std::wstring_view ImagePushCommand::ShortDescription() const
{
    return {L"Pushes an image to a registry."};
}

std::wstring_view ImagePushCommand::LongDescription() const
{
    return {L"Pushes an image to a registry."};
}

void ImagePushCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Image Push subcommand executing..");
}
} // namespace wsl::windows::wslc
