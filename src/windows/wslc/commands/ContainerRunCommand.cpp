/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerRunCommand.cpp

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

namespace wsl::windows::wslc {
// Container Run Command
std::vector<Argument> ContainerRunCommand::GetArguments() const
{
    // clang-format off
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Command),
        Argument::Create(ArgType::ForwardArgs),
        Argument::Create(ArgType::CIDFile),
        Argument::Create(ArgType::Detach),
        Argument::Create(ArgType::DNS),
        Argument::Create(ArgType::DNSDomain),
        Argument::Create(ArgType::DNSOption),
        Argument::Create(ArgType::DNSSearch),
        Argument::Create(ArgType::Entrypoint),
        Argument::Create(ArgType::Env, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::EnvFile),
        Argument::Create(ArgType::Interactive),
        Argument::Create(ArgType::Name),
        Argument::Create(ArgType::NoDNS),
        Argument::Create(ArgType::Progress),
        Argument::Create(ArgType::Publish, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::Pull),
        Argument::Create(ArgType::Remove),
        Argument::Create(ArgType::Scheme),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::TMPFS),
        Argument::Create(ArgType::TTY),
        Argument::Create(ArgType::User),
        Argument::Create(ArgType::Volume),
        Argument::Create(ArgType::Virtual),
    };
    // clang-format on
}

std::wstring ContainerRunCommand::ShortDescription() const
{
    return {L"Run a container."};
}

std::wstring ContainerRunCommand::LongDescription() const
{
    return {L"Runs a container. By default, the container is started in the background; use --detach to run in the foreground."};
}

// clang-format off
void ContainerRunCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context 
        << CreateSession
        << SetContainerOptionsFromArgs
        << RunContainer;
}
// clang-format on
} // namespace wsl::windows::wslc
