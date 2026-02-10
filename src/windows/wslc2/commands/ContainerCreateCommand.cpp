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
            Argument::ForType(ArgType::ImageId),        // Argument
            Argument::ForType(ArgType::ForwardArgs),    // Forward
            Argument::ForType(ArgType::CIDFile),
            Argument::ForType(ArgType::DNS),
            Argument::ForType(ArgType::DNSDomain),
            Argument::ForType(ArgType::DNSOption),
            Argument::ForType(ArgType::DNSSearch),
            Argument::ForType(ArgType::Entrypoint),
            Argument::ForType(ArgType::Env),
            Argument::ForType(ArgType::EnvFile),
            Argument::ForType(ArgType::GroupId),
            Argument::ForType(ArgType::Interactive),
            Argument::ForType(ArgType::Name),
            Argument::ForType(ArgType::NoDNS),
            Argument::ForType(ArgType::Progress),
            Argument::ForType(ArgType::Remove),
            Argument::ForType(ArgType::Scheme),
            Argument::ForType(ArgType::SessionId),
            Argument::ForType(ArgType::TMPFS),
            Argument::ForType(ArgType::TTY),
            Argument::ForType(ArgType::User),
            Argument::ForType(ArgType::Volume),
            Argument::ForType(ArgType::Virtual),
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