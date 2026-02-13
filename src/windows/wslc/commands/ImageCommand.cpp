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
    std::vector<std::unique_ptr<Command>> commands;
    commands.reserve(8);
    commands.push_back(std::make_unique<ImageInspectCommand>(FullName()));
    commands.push_back(std::make_unique<ImageListCommand>(FullName()));
    commands.push_back(std::make_unique<ImageLoadCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePullCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePushCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePruneCommand>(FullName()));
    commands.push_back(std::make_unique<ImageSaveCommand>(FullName()));
    commands.push_back(std::make_unique<ImageTagCommand>(FullName()));
    return commands;
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
