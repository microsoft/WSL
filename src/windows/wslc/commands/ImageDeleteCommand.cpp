/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageDeleteCommand.cpp

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
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Image Delete Command
std::vector<Argument> ImageDeleteCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImageId, true, NO_LIMIT),
        Argument::Create(ArgType::ImageForce),
        Argument::Create(ArgType::NoPrune),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageDeleteCommand::ShortDescription() const
{
    return {L"Delete images."};
}

std::wstring ImageDeleteCommand::LongDescription() const
{
    return {L"Deletes images."};
}

void ImageDeleteCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << DeleteImages;
}
} // namespace wsl::windows::wslc