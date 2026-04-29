/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCreateCommand.cpp

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

namespace wsl::windows::wslc {
// Container Create Command
std::vector<Argument> ContainerCreateCommand::GetArguments() const
{
    // clang-format off
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Command),
        Argument::Create(ArgType::ForwardArgs),
        // Argument::Create(ArgType::CIDFile),
        Argument::Create(ArgType::DNS, false, NO_LIMIT),
        // Argument::Create(ArgType::DNSDomain),
        Argument::Create(ArgType::DNSOption, false, NO_LIMIT),
        Argument::Create(ArgType::DNSSearch, false, NO_LIMIT),
        Argument::Create(ArgType::Domainname),
        Argument::Create(ArgType::Entrypoint),
        Argument::Create(ArgType::Env, false, NO_LIMIT),
        Argument::Create(ArgType::EnvFile, false, NO_LIMIT),
        // Argument::Create(ArgType::GroupId),
        Argument::Create(ArgType::Gpu),
        Argument::Create(ArgType::Hostname),
        Argument::Create(ArgType::Init),
        Argument::Create(ArgType::Interactive),
        Argument::Create(ArgType::Name),
        // Argument::Create(ArgType::NoDNS),
        // Argument::Create(ArgType::Progress),
        Argument::Create(ArgType::Publish, false, NO_LIMIT),
        Argument::Create(ArgType::PublishAll),
        Argument::Create(ArgType::Remove),
        // Argument::Create(ArgType::Scheme),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::TMPFS, false, NO_LIMIT),
        Argument::Create(ArgType::TTY),
        Argument::Create(ArgType::User),
        Argument::Create(ArgType::Volume, false, NO_LIMIT),
        // Argument::Create(ArgType::Virtual),
        Argument::Create(ArgType::WorkDir),
    };
    // clang-format on
}

std::wstring ContainerCreateCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerCreateDesc();
}

std::wstring ContainerCreateCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerCreateLongDesc();
}

// clang-format off
void ContainerCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << SetContainerOptionsFromArgs
        << CreateContainer;
}
// clang-format on
} // namespace wsl::windows::wslc