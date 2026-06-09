/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageInspectCommand.cpp

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
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Image Inspect Command
std::vector<Argument> ImageInspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageInspectCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageInspectDesc();
}

std::wstring ImageInspectCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageInspectLongDesc();
}

void ImageInspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << InspectImages;
}
} // namespace wsl::windows::wslc