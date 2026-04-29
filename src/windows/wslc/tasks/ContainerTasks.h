/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerTasks.h

Abstract:

    Declaration of container command execution tasks.

--*/
#pragma once
#include "CLIExecutionContext.h"
#include "Task.h"

namespace wsl::windows::wslc::task {

struct AttachContainer : public Task
{
    AttachContainer(const std::wstring& containerId) : m_containerId(containerId)
    {
    }
    void operator()(wsl::windows::wslc::execution::CLIExecutionContext& context) const override;

private:
    std::wstring m_containerId;
};

void CreateContainer(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ExecContainer(wsl::windows::wslc::execution::CLIExecutionContext& context);
void GetContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void InspectContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void KillContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ListContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void RemoveContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void RunContainer(wsl::windows::wslc::execution::CLIExecutionContext& context);
void SetContainerOptionsFromArgs(wsl::windows::wslc::execution::CLIExecutionContext& context);
void StartContainer(wsl::windows::wslc::execution::CLIExecutionContext& context);
void StopContainers(wsl::windows::wslc::execution::CLIExecutionContext& context);
void ViewContainerLogs(wsl::windows::wslc::execution::CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
