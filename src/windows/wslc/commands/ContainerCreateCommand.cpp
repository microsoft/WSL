/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerCreateCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "precomp.h"
#include "ContainerModel.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
#include "TablePrinter.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerTasks.h"
#include "Task.h"

using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::wslc::models::ContainerInformation;
using wsl::windows::wslc::services::ContainerService;
using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container Create Command
std::vector<Argument> ContainerCreateCommand::GetArguments() const
{
    // clang-format off
    return {
        Argument::Create(ArgType::ImageId, true),
        Argument::Create(ArgType::Command),
        Argument::Create(ArgType::ForwardArgs),
        Argument::Create(ArgType::CIDFile),
        Argument::Create(ArgType::DNS),
        Argument::Create(ArgType::DNSDomain),
        Argument::Create(ArgType::DNSOption),
        Argument::Create(ArgType::DNSSearch),
        Argument::Create(ArgType::Entrypoint),
        Argument::Create(ArgType::Env, false, NO_LIMIT),
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
    // clang-format on
}

std::wstring ContainerCreateCommand::ShortDescription() const
{
    return {L"Create a container."};
}

std::wstring ContainerCreateCommand::LongDescription() const
{
    return {L"Creates a container."};
}

// clang-format off
void ContainerCreateCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context
        << CreateSession
        << SetCreateContainerOptionsFromArgs
        << CreateContainer;
}
// clang-format on
} // namespace wsl::windows::wslc