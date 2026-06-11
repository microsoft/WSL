/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkListCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "NetworkCommand.h"
#include "CLIExecutionContext.h"
#include "SessionTasks.h"
#include "NetworkTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Network List Command
std::vector<Argument> NetworkListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::Quiet, false, std::nullopt, Localization::WSLCCLI_NetworkListQuietArgDesc()),
        Argument::Create(ArgType::Session),
    };
}

std::wstring NetworkListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_NetworkListDesc();
}

std::wstring NetworkListCommand::LongDescription() const
{
    return Localization::WSLCCLI_NetworkListLongDesc();
}

void NetworkListCommand::ValidateArgumentsInternal(const ArgMap& execArgs) const
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

void NetworkListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context << CreateSession //
            << GetNetworks   //
            << ListNetworks;
}
} // namespace wsl::windows::wslc
