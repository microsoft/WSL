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

using wsl::windows::wslc::execution::CLIExecutionContext;

namespace wsl::windows::wslc::task {

struct AttachContainer : public Task
{
    AttachContainer(const std::wstring& containerId) : m_containerId(containerId)
    {
    }
    void operator()(CLIExecutionContext& context) const override;

private:
    std::wstring m_containerId;
};

void CreateContainer(CLIExecutionContext& context);
void ExecContainer(CLIExecutionContext& context);
void GetContainers(CLIExecutionContext& context);
void InspectContainers(CLIExecutionContext& context);
void KillContainers(CLIExecutionContext& context);
void ListContainers(CLIExecutionContext& context);
void RemoveContainers(CLIExecutionContext& context);
void RunContainer(CLIExecutionContext& context);
void SetContainerOptionsFromArgs(CLIExecutionContext& context);
void StartContainer(CLIExecutionContext& context);
void StopContainers(CLIExecutionContext& context);
void ViewContainerLogs(CLIExecutionContext& context);
void PruneContainers(CLIExecutionContext& context);
} // namespace wsl::windows::wslc::task
