/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImagePullCommand.cpp

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
// Image Pull Command
std::vector<Argument> ImagePullCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true),
        // Argument::Create(ArgType::Scheme),
        // Argument::Create(ArgType::Progress),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImagePullCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImagePullDesc();
}

std::wstring ImagePullCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImagePullLongDesc();
}

void ImagePullCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << PullImage;
}
} // namespace wsl::windows::wslc