/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageLoadCommand.cpp

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
// Image Load Command
std::vector<Argument> ImageLoadCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Input, true), // Argument
        Argument::Create(ArgType::SessionId)};
}

std::wstring_view ImageLoadCommand::ShortDescription() const
{
    return {L"Loads an image from a tar archive file on disk. "};
}

std::wstring_view ImageLoadCommand::LongDescription() const
{
    return {L"Loads an image from a tar archive file on disk. "};
}

void ImageLoadCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Image Load subcommand executing..");
}
} // namespace wsl::windows::wslc
