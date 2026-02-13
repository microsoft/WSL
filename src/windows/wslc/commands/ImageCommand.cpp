/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
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
// Image Root Command
std::vector<std::unique_ptr<Command>> ImageCommand::GetCommands() const
{
    return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
        std::make_unique<ImageInspectCommand>(FullName()),
        std::make_unique<ImageListCommand>(FullName()),
        std::make_unique<ImageLoadCommand>(FullName()),
        std::make_unique<ImagePullCommand>(FullName()),
        std::make_unique<ImagePushCommand>(FullName()),
        std::make_unique<ImagePruneCommand>(FullName()),
        std::make_unique<ImageSaveCommand>(FullName()),
        std::make_unique<ImageTagCommand>(FullName()),
    });
}

std::vector<Argument> ImageCommand::GetArguments() const
{
    return {};
}

std::wstring_view ImageCommand::ShortDescription() const
{
    return {L"Image command"};
}

std::wstring_view ImageCommand::LongDescription() const
{
    return {L"Image command for demonstration purposes."};
}

void ImageCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Image base command executing..", stdout);
}
} // namespace wsl::windows::wslc
