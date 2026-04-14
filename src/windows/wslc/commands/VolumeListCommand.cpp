/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VolumeListCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "VolumeCommand.h"
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "VolumeTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Volume List Command
std::vector<Argument> VolumeListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::Quiet, false, std::nullopt, L"Outputs the volume names only"),
        Argument::Create(ArgType::Session),
    };
}

std::wstring VolumeListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_VolumeListDesc();
}

std::wstring VolumeListCommand::LongDescription() const
{
    return Localization::WSLCCLI_VolumeListLongDesc();
}

void VolumeListCommand::ValidateArgumentsInternal(const ArgMap& execArgs) const
{
    if (execArgs.Contains(ArgType::Format))
    {
        auto format = execArgs.Get<ArgType::Format>();
        if (!IsEqual(format, L"json") && !IsEqual(format, L"table"))
        {
            throw CommandException(Localization::WSLCCLI_InvalidFormatError());
        }
    }
}

void VolumeListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << GetVolumes    //
            << ListVolumes;
}
} // namespace wsl::windows::wslc
