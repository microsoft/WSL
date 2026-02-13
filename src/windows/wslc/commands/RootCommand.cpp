// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "RootCommand.h"
#include "TaskBase.h"

// Include all commands that parent to the root. 
#include "DiagCommand.h"

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<DiagCommand>(FullName()),
            std::make_unique<DiagListCommand>(FullName()),
        });
    }

    std::vector<Argument> RootCommand::GetArguments() const
    {
        return
        {
            Argument::Create(ArgType::Info),
        };
    }

    std::wstring_view RootCommand::ShortDescription() const
    {
        return { L"WSLC is the Windows Subsystem for Linux Container CLI tool." };
    }

    std::wstring_view RootCommand::LongDescription() const
    {
        return { L"WSLC is the Windows Subsystem for Linux Container CLI tool. It enables management and interaction with WSL containers from the command line." };
    }

    void RootCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        OutputHelp();
    }
}

