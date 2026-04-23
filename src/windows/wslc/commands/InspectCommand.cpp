/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InspectCommand.cpp

Abstract:

    Implementation of the inspect command.
--*/
#include "InspectCommand.h"
#include "SessionTasks.h"
#include "InspectTasks.h"

using namespace wsl::shared;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {

std::vector<Argument> InspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ObjectId, true, NO_LIMIT),
        Argument::Create(ArgType::Type),
        Argument::Create(ArgType::Session),
    };
}

std::wstring InspectCommand::ShortDescription() const
{
    return {Localization::WSLCCLI_InspectDesc()};
}

std::wstring InspectCommand::LongDescription() const
{
    return {Localization::WSLCCLI_InspectLongDesc()};
}

void InspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << Inspect;
}
} // namespace wsl::windows::wslc
