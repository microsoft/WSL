/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageBuildCommand.cpp

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
// Image Build Command
std::vector<Argument> ImageBuildCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Path, true),
        Argument::Create(ArgType::BuildArg, false, NO_LIMIT),
        Argument::Create(ArgType::File),
        Argument::Create(ArgType::NoCache),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::Tag, false, NO_LIMIT),
        Argument::Create(ArgType::Verbose),
    };
}

std::wstring ImageBuildCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageBuildDesc();
}

std::wstring ImageBuildCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageBuildLongDesc();
}

void ImageBuildCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << BuildImage;
}
} // namespace wsl::windows::wslc