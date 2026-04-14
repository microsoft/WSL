/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#include "CLIExecutionContext.h"
#include "ImageCommand.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Image Root Command
std::vector<std::unique_ptr<Command>> ImageCommand::GetCommands() const
{
    std::vector<std::unique_ptr<Command>> commands;
    commands.push_back(std::make_unique<ImageBuildCommand>(FullName()));
    commands.push_back(std::make_unique<ImageRemoveCommand>(FullName()));
    commands.push_back(std::make_unique<ImageInspectCommand>(FullName()));
    commands.push_back(std::make_unique<ImageListCommand>(FullName()));
    commands.push_back(std::make_unique<ImageLoadCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePruneCommand>(FullName()));
    commands.push_back(std::make_unique<ImagePullCommand>(FullName()));
    commands.push_back(std::make_unique<ImageSaveCommand>(FullName()));
    commands.push_back(std::make_unique<ImageTagCommand>(FullName()));
    return commands;
}

std::vector<Argument> ImageCommand::GetArguments() const
{
    return {};
}

std::wstring ImageCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageCommandDesc();
}

std::wstring ImageCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageCommandLongDesc();
}

void ImageCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    OutputHelp();
}
} // namespace wsl::windows::wslc