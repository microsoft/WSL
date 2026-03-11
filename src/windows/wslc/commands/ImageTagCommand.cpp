/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageTagCommand.cpp

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
// Image Tag Command
std::vector<Argument> ImageTagCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Source, true),
        Argument::Create(ArgType::Target, true),
    };
}

std::wstring ImageTagCommand::ShortDescription() const
{
    return {L"Tags an existing image."};
}

std::wstring ImageTagCommand::LongDescription() const
{
    return {L"Tags an existing image."};
}

void ImageTagCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << TagImage;
}
} // namespace wsl::windows::wslc