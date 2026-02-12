// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "RootCommand.h"
#include "TaskBase.h"

// Include all commands that parent to the root. 
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "RegistryCommand.h"
#include "SessionCommand.h"
#include "VolumeCommand.h"

#ifdef _DEBUG
#include "TestCommand.h"
#endif

using namespace wsl::shared;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc
{
    std::vector<std::unique_ptr<Command>> RootCommand::GetCommands() const
    {
        return InitializeFromMoveOnly<std::vector<std::unique_ptr<Command>>>({
            std::make_unique<ContainerCommand>(FullName()),
            std::make_unique<ImageCommand>(FullName()),
            std::make_unique<RegistryCommand>(FullName()),
            std::make_unique<SessionCommand>(FullName()),
            std::make_unique<VolumeCommand>(FullName()),
            std::make_unique<ContainerCreateCommand>(FullName()),
            std::make_unique<ContainerDeleteCommand>(FullName()),
            std::make_unique<ContainerExecCommand>(FullName()),
            std::make_unique<ContainerInspectCommand>(FullName()),
            std::make_unique<ContainerKillCommand>(FullName()),
            std::make_unique<ContainerListCommand>(FullName()),
            std::make_unique<ContainerRunCommand>(FullName()),
            std::make_unique<ContainerStartCommand>(FullName()),
            std::make_unique<ContainerStopCommand>(FullName()),

#ifdef _DEBUG
            std::make_unique<TestCommand>(FullName()),
#endif
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

