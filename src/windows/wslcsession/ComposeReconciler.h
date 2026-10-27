// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ComposeCancellation.h"
#include "ComposeSpec.h"
#include "ExecutionContext.h"
#include "wslc.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCSession;
class WSLCContainer;

struct ComposeExecutionResult
{
    std::vector<WSLCContainerEntry> AffectedContainers;
};

// Invoked before resource work; Current identifies the resource's ordinal within its operation group.
using ComposeProgressReporter =
    std::function<void(std::string_view operation, std::string_view resourceKey, ULONGLONG current, ULONGLONG total, std::string_view unit)>;

class ComposeReconciler
{
public:
    explicit ComposeReconciler(WSLCSession& session) noexcept;

    // projectKey is normalized within the owning session; Create and Up require desiredProject.
    ComposeExecutionResult Execute(
        WSLCComposeAction action,
        const std::optional<ComposeSpec>& desiredProject,
        std::string_view projectKey,
        const WSLCComposeActionOptions& actionOptions,
        const ComposeCancellation& cancellation,
        const ComposeProgressReporter& progressReporter,
        std::optional<wsl::windows::common::Error>& primaryError);

private:
    struct ProjectLock
    {
        ProjectLock()
        {
            Lock.create();
        }

        wil::unique_mutex Lock;
        bool Active{};
    };

    std::shared_ptr<ProjectLock> ResolveProjectLock(std::string_view projectKey);
    static std::vector<WSLCContainerEntry> ObserveContainers(const std::vector<Microsoft::WRL::ComPtr<WSLCContainer>>& containers);
    std::vector<Microsoft::WRL::ComPtr<WSLCContainer>> Create(
        const ComposeSpec& project, const ComposeCancellation& cancellation, const ComposeProgressReporter& progressReporter);
    std::vector<Microsoft::WRL::ComPtr<WSLCContainer>> Up(
        const ComposeSpec& project,
        std::vector<Microsoft::WRL::ComPtr<WSLCContainer>> containers,
        const ComposeCancellation& cancellation,
        const ComposeProgressReporter& progressReporter);
    static void Start(const std::vector<Microsoft::WRL::ComPtr<WSLCContainer>>& containers, const ComposeCancellation& cancellation, const ComposeProgressReporter& progressReporter);
    static void Stop(
        const std::vector<Microsoft::WRL::ComPtr<WSLCContainer>>& containers,
        ULONG timeout,
        const ComposeCancellation& cancellation,
        const ComposeProgressReporter& progressReporter,
        std::optional<wsl::windows::common::Error>& primaryError);
    static std::vector<WSLCContainerEntry> Remove(
        const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers,
        const ComposeCancellation& cancellation,
        const ComposeProgressReporter& progressReporter);

    WSLCSession& m_session;
    std::mutex m_projectLocksLock;
    std::unordered_map<std::string, std::shared_ptr<ProjectLock>> m_projectLocks;
};

} // namespace wsl::windows::service::wslc
