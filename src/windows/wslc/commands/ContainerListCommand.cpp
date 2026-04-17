/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerListCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "ContainerCommand.h"
#include "CLIExecutionContext.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;
using namespace wsl::shared;
using namespace wsl::shared::string;

namespace wsl::windows::wslc {
// Container List Command
std::vector<Argument> ContainerListCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::All),
        Argument::Create(ArgType::Format),
        Argument::Create(ArgType::NoTrunc),
        Argument::Create(ArgType::Quiet),
        Argument::Create(ArgType::Session),
    };
}

std::wstring ContainerListCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerListDesc();
}

std::wstring ContainerListCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerListLongDesc();
}

void ContainerListCommand::ValidateArgumentsInternal(const ArgMap& execArgs) const
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

// clang-format off
void ContainerListCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << GetContainers
        << ListContainers;
}
// clang-format on
} // namespace wsl::windows::wslc