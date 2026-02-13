/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    RegistryLoginCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "RegistryCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Registry Login Command
std::vector<Argument> RegistryLoginCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::Server, true), // Argument
        Argument::Create(ArgType::PasswordStdin),
        Argument::Create(ArgType::Scheme),
        Argument::Create(ArgType::SessionId),
        Argument::Create(ArgType::UserName),
    };
}

std::wstring_view RegistryLoginCommand::ShortDescription() const
{
    return {L"Authenticates to a registry."};
}

std::wstring_view RegistryLoginCommand::LongDescription() const
{
    return {L"Authenticates to a registry."};
}

void RegistryLoginCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    PrintMessage(L"Registry Login subcommand executing..");
}
} // namespace wsl::windows::wslc
