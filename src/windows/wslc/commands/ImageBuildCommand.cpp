/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageBuildCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "ImageCommand.h"
#include "CLIExecutionContext.h"
#include "ImageTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Image Build Command
std::vector<Argument> ImageBuildCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Path, true),
        Argument::Create(ArgType::Tag),
        Argument::Create(ArgType::File),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageBuildCommand::ShortDescription() const
{
    return {L"Build an image from a Dockerfile."};
}

std::wstring ImageBuildCommand::LongDescription() const
{
    return {L"Builds an image from a Dockerfile and a build context directory."};
}

void ImageBuildCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << BuildImage;
}
} // namespace wsl::windows::wslc