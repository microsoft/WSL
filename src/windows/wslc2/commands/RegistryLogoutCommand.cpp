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
    // Registry Logout Command
    std::vector<Argument> RegistryLogoutCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::Registry, true),        // Argument
            Argument::Create(ArgType::SessionId)
        };
    }

    std::wstring_view RegistryLogoutCommand::ShortDescription() const
    {
        return {L"Logs out of the registry."};
    }

    std::wstring_view RegistryLogoutCommand::LongDescription() const
    {
        return {L"Logs out of the registry. The cached/stored credentials must be removed. "};
    }

    void RegistryLogoutCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Registry Logout subcommand executing..");
    }
}
