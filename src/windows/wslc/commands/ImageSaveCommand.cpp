/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageSaveCommand.cpp

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
// Image Save Command
std::vector<Argument> ImageSaveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Output, true),
    };
}

std::wstring ImageSaveCommand::ShortDescription() const
{
    return {L"Saves image."};
}

std::wstring ImageSaveCommand::LongDescription() const
{
    return {L"Saves image."};
}

void ImageSaveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << SaveImage;
}
} // namespace wsl::windows::wslc