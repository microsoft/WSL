/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerExecCommand.cpp

Abstract:

    Implementation of command execution logic.

--*/

#include "precomp.h"
#include "ContainerModel.h"
#include "ContainerCommand.h"
#include "ContainerService.h"
#include "CLIExecutionContext.h"
#include "ExecutionContextData.h"
#include "ContainerTasks.h"
#include "SessionTasks.h"
#include "Task.h"

using wsl::windows::common::string::WideToMultiByte;
using wsl::windows::common::wslutil::PrintMessage;
using wsl::windows::wslc::models::ContainerInformation;
using wsl::windows::wslc::services::ContainerService;
using namespace wsl::shared;
using namespace wsl::windows::wslc::execution;
using namespace wsl::windows::wslc::task;

namespace wsl::windows::wslc {
// Container Exec Command
std::vector<Argument> ContainerExecCommand::GetArguments() const
{
    return {
        Argument::Create(ArgType::ContainerId, true),
        Argument::Create(ArgType::Command, true),
        Argument::Create(ArgType::ForwardArgs, std::nullopt, std::nullopt, L"Arguments to pass to the command being executed inside the container"),
        Argument::Create(ArgType::Detach),
        Argument::Create(ArgType::Env, std::nullopt, NO_LIMIT),
        Argument::Create(ArgType::EnvFile),
        Argument::Create(ArgType::Interactive),
        Argument::Create(ArgType::Session),
        Argument::Create(ArgType::TTY),
        Argument::Create(ArgType::User),
    };
}

std::wstring ContainerExecCommand::ShortDescription() const
{
    return {L"Execute a command in a running container."};
}

std::wstring ContainerExecCommand::LongDescription() const
{
    return {
        L"Executes a command in a running container."};
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
