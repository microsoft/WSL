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
    // Container Run Command
    std::vector<Argument> ContainerRunCommand::GetArguments() const
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
            Argument::ForType(ArgType::Interactive),
            Argument::ForType(ArgType::Name),
            Argument::ForType(ArgType::NoDNS),
            Argument::ForType(ArgType::Progress),
            Argument::ForType(ArgType::Publish),
            Argument::ForType(ArgType::Pull),
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

    std::wstring_view ContainerRunCommand::ShortDescription() const
    {
        return { L"Create and run a new container from an image." };
    }

    std::wstring_view ContainerRunCommand::LongDescription() const
    {
        return { L"Create and run a new container from an image." };
    }

    void ContainerRunCommand::ValidateArgumentsInternal(Args& execArgs) const
    {
        // Argument validation is done in ArgumentValidation.cpp, including
        // cross-argument validation, but this method is for command-specific validation.
    }

    void ContainerRunCommand::ExecuteInternal(CLIExecutionContext& context) const
    {
        PrintMessage(L"Container Run subcommand executing..", stdout);
    }
}