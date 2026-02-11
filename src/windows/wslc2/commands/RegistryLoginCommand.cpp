// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
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

namespace wsl::windows::wslc
{
    // Registry Login Command
    std::vector<Argument> RegistryLoginCommand::GetArguments() const
    {
        return {
            Argument::ForType(ArgType::Server),
            Argument::ForType(ArgType::PasswordStdin),
            Argument::ForType(ArgType::Scheme),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::UserName),
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
}
