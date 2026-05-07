/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageRemoveCommand.cpp

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
// Image Remove Command
std::vector<Argument> ImageRemoveCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::ImageForce),
        Argument::Create(ArgType::NoPrune),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageRemoveCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageRemoveDesc();
}

std::wstring ImageRemoveCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageRemoveLongDesc();
}

void ImageRemoveCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << DeleteImage;
}
} // namespace wsl::windows::wslc