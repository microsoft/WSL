/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImagePruneCommand.cpp

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
// Image Prune Command
std::vector<Argument> ImagePruneCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::ImageForce),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImagePruneCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImagePruneDesc();
}

std::wstring ImagePruneCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImagePruneLongDesc();
}

void ImagePruneCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << PruneImages;
}
} // namespace wsl::windows::wslc
