/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerExecCommand.cpp

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
// Container Exec Command
std::vector<Argument> ContainerExecCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Command, true),
        Argument::Create(ArgType::ForwardArgs, std::nullopt, std::nullopt, Localization::WSLCCLI_ContainerExecForwardArgsDescription()),
        Argument::Create(ArgType::Detach),
        Argument::Create(ArgType::Env, false, NO_LIMIT),
        Argument::Create(ArgType::EnvFile, false, NO_LIMIT),
        Argument::Create(ArgType::Interactive),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::TTY),
        // Argument::Create(ArgType::User),
        Argument::Create(ArgType::WorkDir),
    };
}

std::wstring ContainerExecCommand::ShortDescription() const
{
    return Localization::WSLCCLI_ContainerExecDesc();
}

std::wstring ContainerExecCommand::LongDescription() const
{
    return Localization::WSLCCLI_ContainerExecLongDesc();
}
// clang-format off
void ContainerExecCommand::ExecuteInternal(CLIExecutionContext& context) const
{
    context 
        << CreateSession
        << SetContainerOptionsFromArgs
        << ExecContainer;
}
// clang-format on
} // namespace wsl::windows::wslc
