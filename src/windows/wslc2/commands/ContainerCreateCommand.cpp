// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerCommand.h"
#include "CommonTasks.h"
#include "TaskBase.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc
{
    // Container Create Command
    std::vector<Argument> ContainerCreateCommand::GetArguments() const
    {
        return {
            Argument::Create(ArgType::ImageId),        // Argument
            Argument::Create(ArgType::ForwardArgs),    // Forward
            Argument::Create(ArgType::CIDFile),
            Argument::Create(ArgType::DNS),
            Argument::Create(ArgType::DNSDomain),
            Argument::Create(ArgType::DNSOption),
            Argument::Create(ArgType::DNSSearch),
            Argument::Create(ArgType::Entrypoint),
            Argument::Create(ArgType::Env),
            Argument::Create(ArgType::EnvFile),
            Argument::Create(ArgType::GroupId),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::Name),
            Argument::Create(ArgType::NoDNS),
            Argument::Create(ArgType::Progress),
            Argument::Create(ArgType::Remove),
            Argument::Create(ArgType::Scheme),
            Argument::Create(ArgType::SessionId),
            Argument::Create(ArgType::TMPFS),
            Argument::Create(ArgType::TTY),
            Argument::Create(ArgType::User),
            Argument::Create(ArgType::Volume),
            Argument::Create(ArgType::Virtual),
        };
    }

    std::wstring_view ContainerCreateCommand::ShortDescription() const
    {
        return { L"Creates a container but does not start it." };
    }

    std::wstring_view ContainerCreateCommand::LongDescription() const
    {
        return { L"Creates a container but does not start it. Most of the same options as 'wslc.exe run'" };
    }

    void ContainerCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Create subcommand executing..", stdout);
    }
}