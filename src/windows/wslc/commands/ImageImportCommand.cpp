/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageImportCommand.cpp

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
// Image Import Command
std::vector<Argument> ImageImportCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ImportFile, true),
        Argument::Create(ArgType::ImageId),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ImageImportCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ImageImportDesc();
}

std::wstring ImageImportCommand::LongDescription() const
{
    return Localization::WSLCCLI_ImageImportLongDesc();
}

void ImageImportCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context              //
        << CreateSession //
        << ImportImage;
}
} // namespace wsl::windows::wslc
