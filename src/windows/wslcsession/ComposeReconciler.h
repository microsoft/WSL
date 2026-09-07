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
    struct ProjectLock
    {
        std::timed_mutex Lock;
    };

    std::shared_ptr<ProjectLock> ResolveProjectLock(std::string_view ProjectKey);
    static std::vector<WSLCContainerEntry> ObserveContainers(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers);
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Create(const ComposeSpec& Project, HANDLE CancelEvent);
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Up(
        const ComposeSpec& Project, std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Containers, HANDLE CancelEvent);
    static void Start(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers, HANDLE CancelEvent);
    static void Stop(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers, ULONG Timeout, HANDLE CancelEvent);
    static void CheckCancelled(HANDLE CancelEvent);

    WSLCSession& m_session;
    std::mutex m_projectLocksLock;
    std::unordered_map<std::string, std::shared_ptr<ProjectLock>> m_projectLocks;
};

} // namespace wsl::windows::service::wslc
