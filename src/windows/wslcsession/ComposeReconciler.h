// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeSpec.h"
#include "wslc.h"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCSession;

struct ComposeExecutionResult
{
    std::string ProjectKey;
    std::vector<WSLCContainerEntry> AffectedContainers;
};

class ComposeReconciler
{
public:
    explicit ComposeReconciler(WSLCSession& Session) noexcept;

    ComposeExecutionResult Execute(
        WSLCComposeAction Action, const ComposeSpec* DesiredProject, std::string_view ProjectKey, const WSLCComposeActionOptions& ActionOptions, HANDLE CancelEvent);

private:
    struct ProjectState
    {
        std::timed_mutex Lock;
        ComposeSpec Spec;
        std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Containers;
    };

    std::shared_ptr<ProjectState> ResolveProject(WSLCComposeAction Action, const ComposeSpec* DesiredProject, std::string_view ProjectKey);
    static std::vector<WSLCContainerEntry> ObserveContainers(const ProjectState& Project);
    void Create(ProjectState& Project, HANDLE CancelEvent);
    void Up(ProjectState& Project, HANDLE CancelEvent);
    static void Start(ProjectState& Project, HANDLE CancelEvent);
    static void Stop(ProjectState& Project, ULONG Timeout, HANDLE CancelEvent);
    static void CheckCancelled(HANDLE CancelEvent);

    WSLCSession& m_session;
    std::mutex m_projectsLock;
    std::unordered_map<std::string, std::shared_ptr<ProjectState>> m_projects;
};

} // namespace wsl::windows::service::wslc
