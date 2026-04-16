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

using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {

std::vector<Argument> InspectCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Type),
        Argument::Create(ArgType::ObjectId, true, NO_LIMIT),
        Argument::Create(ArgType::Session),
    };
}

std::wstring InspectCommand::ShortDescription() const
{
    return {L"Inspect objects."};
}

std::wstring InspectCommand::LongDescription() const
{
    return {L"Inspects objects."};
}

void InspectCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << Inspect;
}
} // namespace wsl::windows::wslc
