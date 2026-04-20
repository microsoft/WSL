/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImagePushCommand.cpp

Abstract:

    Implementation of the image push command.

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
// Image Push Command
std::vector<Argument> ImagePushCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImagePushCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImagePushDesc();
}

std::wstring ImagePushCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImagePushLongDesc();
}

void ImagePushCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << PushImage;
}
} // namespace wsl::windows::wslc
