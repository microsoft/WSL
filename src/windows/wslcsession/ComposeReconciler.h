// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeSpec.h"
#include "wslc.h"
#include <functional>
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

using ComposeProgressReporter =
    std::function<void(std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit)>;

class ComposeReconciler
{
public:
    explicit ComposeReconciler(WSLCSession& session) noexcept;

    ComposeExecutionResult Execute(
        WSLCComposeAction action,
        const ComposeSpec* desiredProject,
        std::string_view projectKey,
        const WSLCComposeActionOptions& actionOptions,
        HANDLE cancelEvent,
        const ComposeProgressReporter& progressReporter);

private:
    struct ProjectLock
    {
        std::timed_mutex Lock;
    };

    std::shared_ptr<ProjectLock> ResolveProjectLock(std::string_view projectKey);
    static std::vector<WSLCContainerEntry> ObserveContainers(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers);
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Create(const ComposeSpec& project, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter);
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Up(
        const ComposeSpec& project,
        std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> containers,
        HANDLE cancelEvent,
        const ComposeProgressReporter& progressReporter);
    static void Start(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter);
    static void Stop(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, ULONG timeout, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter);
    static std::vector<WSLCContainerEntry> Remove(
        const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter);
    static void CheckCancelled(HANDLE cancelEvent);

    WSLCSession& m_session;
    std::mutex m_projectLocksLock;
    std::unordered_map<std::string, std::shared_ptr<ProjectLock>> m_projectLocks;
};

} // namespace wsl::windows::service::wslc
