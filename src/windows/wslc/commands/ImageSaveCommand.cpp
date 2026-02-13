/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageSaveCommand.cpp

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
// Image Save Command
std::vector<Argument> ImageSaveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true), // Argument
        Argument::Create(ArgType::Output, true),
        Argument::Create(ArgType::SessionId)};
}

std::wstring_view ImageSaveCommand::ShortDescription() const
{
    return {L"Saves an image to a tar archive file on disk. "};
}

std::wstring_view ImageSaveCommand::LongDescription() const
{
    return {L"Saves an image to a tar archive file on disk. "};
}

void ImageSaveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Image Save subcommand executing..");
}
} // namespace wsl::windows::wslc
