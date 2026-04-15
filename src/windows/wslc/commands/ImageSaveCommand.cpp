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
using namespace wsl::shared;

namespace wsl::windows::wslc {
// Image Save Command
std::vector<Argument> ImageSaveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Output),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageSaveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageSaveDesc();
}

std::wstring ImageSaveCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageSaveLongDesc();
}

void ImageSaveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << SaveImage;
}
} // namespace wsl::windows::wslc