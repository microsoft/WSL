/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageLoadCommand.cpp

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
// Image Load Command
std::vector<Argument> ImageLoadCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Input),
    };
}

std::wstring ImageLoadCommand::ShortDescription() const
{
    return {L"Load images."};
}

std::wstring ImageLoadCommand::LongDescription() const
{
    return {L"Loads images."};
}

void ImageLoadCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << LoadImage;
}
} // namespace wsl::windows::wslc