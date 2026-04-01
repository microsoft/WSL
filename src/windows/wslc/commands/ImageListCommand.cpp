/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageListCommand.cpp

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
// Image List Command
std::vector<Argument> ImageListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::NoTrunc),
        Argument::Create(ArgType::Quiet),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Verbose)};
}

std::wstring ImageListCommand::ShortDescription() const
{
    return {L"List images."};
}

std::wstring ImageListCommand::LongDescription() const
{
    return {L"Lists images."};
}

void ImageListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << GetImages     //
        << ListImages;
}
} // namespace wsl::windows::wslc