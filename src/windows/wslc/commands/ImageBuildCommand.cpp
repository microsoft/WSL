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
        Argument::Create(ArgType::Path, {.Required = true}),
        Argument::Create(ArgType::BuildArg, {.Limit = Limit::Unlimited}),
        Argument::Create(ArgType::BuildPull),
        Argument::Create(ArgType::BuildTarget),
        Argument::Create(ArgType::File),
        Argument::Create(ArgType::IidFile),
        Argument::Create(ArgType::BuildLabel, {.Limit = Limit::Unlimited}),
        Argument::Create(ArgType::NoCache),
        Argument::Create(ArgType::BuildOutput, {.Desc = Localization::WSLCCLI_BuildOutputArgDescription()}),
        Argument::Create(ArgType::Progress),
        Argument::Create(ArgType::Secret, {.Limit = Limit::Unlimited}),
        Argument::Create(ArgType::Tag, {.Limit = Limit::Unlimited}),
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
    context               //
        << ResolveSession //
        << BuildImage;
}
} // namespace wsl::windows::wslc