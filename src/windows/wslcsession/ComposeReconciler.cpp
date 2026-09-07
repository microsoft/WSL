// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeReconciler.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

ComposeReconciler::ComposeReconciler(WSLCSession& session) noexcept : m_session(session)
{
}

ComposeExecutionResult ComposeReconciler::Execute(
    WSLCComposeAction action,
    const ComposeSpec* desiredProject,
    std::string_view projectKey,
    const WSLCComposeActionOptions& actionOptions,
    HANDLE cancelEvent,
    const ComposeProgressReporter& progressReporter)
{
    CheckCancelled(cancelEvent);
    auto projectLockEntry = ResolveProjectLock(projectKey);
    std::unique_lock projectLock(projectLockEntry->Lock, std::defer_lock);
    while (!projectLock.try_lock_for(std::chrono::milliseconds{100}))
    {
        CheckCancelled(cancelEvent);
    }
    CheckCancelled(cancelEvent);
    auto containers = m_session.DiscoverComposeContainers(projectKey);
    CheckCancelled(cancelEvent);

    switch (action)
    {
    case WSLCComposeActionCreate:
        THROW_HR_IF_NULL(E_INVALIDARG, desiredProject);
        if (containers.empty())
        {
            containers = Create(*desiredProject, cancelEvent, progressReporter);
        }
        break;

    case WSLCComposeActionUp:
        THROW_HR_IF_NULL(E_INVALIDARG, desiredProject);
        containers = Up(*desiredProject, std::move(containers), cancelEvent, progressReporter);
        break;

    case WSLCComposeActionStart:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Start(containers, cancelEvent, progressReporter);
        break;

    case WSLCComposeActionAttach:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        break;

    case WSLCComposeActionStop:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Stop(containers, actionOptions.Value.Stop.TimeoutSeconds, cancelEvent, progressReporter);
        break;

    case WSLCComposeActionRemove:
        Remove(containers, cancelEvent, progressReporter);
        containers.clear();
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    return {
        .ProjectKey = std::string{projectKey},
        .AffectedContainers = ObserveContainers(containers),
    };
}

std::shared_ptr<ComposeReconciler::ProjectLock> ComposeReconciler::ResolveProjectLock(std::string_view projectKey)
{
    std::lock_guard projectLocksLock(m_projectLocksLock);
    const auto existing = m_projectLocks.find(std::string{projectKey});
    if (existing != m_projectLocks.end())
    {
        return existing->second;
    }

    auto projectLock = std::make_shared<ProjectLock>();
    const auto [entry, inserted] = m_projectLocks.emplace(std::string{projectKey}, projectLock);
    WI_ASSERT(inserted);
    return entry->second;
}

std::vector<WSLCContainerEntry> ComposeReconciler::ObserveContainers(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers)
{
    std::vector<WSLCContainerEntry> result;
    result.reserve(containers.size());
    for (const auto& container : containers)
    {
        WSLCContainerEntry entry{};
        wil::unique_cotaskmem_ansistring name;
        THROW_IF_FAILED(container->GetName(&name));
        THROW_IF_FAILED(container->GetId(entry.Id));
        THROW_IF_FAILED(container->GetState(&entry.State));
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(entry.Name, name.get()) != 0);

        wil::unique_cotaskmem_ansistring inspect;
        THROW_IF_FAILED(container->Inspect(&inspect));
        const auto json = nlohmann::json::parse(inspect.get());
        const auto image = json.value("Image", std::string{});
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(entry.Image, image.c_str()) != 0);
        result.emplace_back(entry);
    }

    return result;
}

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Create(const ComposeSpec& project, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter)
{
    return m_session.CreateComposeContainers(project, cancelEvent, progressReporter);
}

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Up(
    const ComposeSpec& project, std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> containers, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter)
{
    for (size_t index = 0; index < containers.size(); ++index)
    {
        CheckCancelled(cancelEvent);
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("remove", name.get(), index + 1, containers.size(), "container");
            CheckCancelled(cancelEvent);
        }

        const HRESULT result = container->Delete(WSLCDeleteFlagsForce);
        THROW_IF_FAILED_EXCEPT(result, RPC_E_DISCONNECTED);
    }

    CheckCancelled(cancelEvent);
    auto result = m_session.CreateComposeContainers(project, cancelEvent, progressReporter);
    Start(result, cancelEvent, progressReporter);
    return result;
}

void ComposeReconciler::Start(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter)
{
    for (size_t index = 0; index < containers.size(); ++index)
    {
        CheckCancelled(cancelEvent);
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("start", name.get(), index + 1, containers.size(), "container");
            CheckCancelled(cancelEvent);
        }

        const HRESULT result = container->Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
        THROW_IF_FAILED_EXCEPT(result, WSLC_E_CONTAINER_IS_RUNNING);
    }
}

void ComposeReconciler::Stop(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, ULONG timeout, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter)
{
    THROW_HR_IF(E_INVALIDARG, timeout > LONG_MAX);

    HRESULT firstFailure = S_OK;
    for (size_t index = 0; index < containers.size(); ++index)
    {
        CheckCancelled(cancelEvent);
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("stop", name.get(), index + 1, containers.size(), "container");
            CheckCancelled(cancelEvent);
        }

        const HRESULT result = container->Stop(WSLCSignalSIGTERM, static_cast<LONG>(timeout));
        if (FAILED(result) && result != WSLC_E_CONTAINER_NOT_RUNNING && SUCCEEDED(firstFailure))
        {
            firstFailure = result;
        }
    }

    THROW_IF_FAILED(firstFailure);
}

void ComposeReconciler::Remove(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, HANDLE cancelEvent, const ComposeProgressReporter& progressReporter)
{
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> stoppedContainers;
    for (const auto& container : containers)
    {
        WSLCContainerState state{};
        THROW_IF_FAILED(container->GetState(&state));
        if (state != WslcContainerStateRunning && state != WslcContainerStateDeleted)
        {
            stoppedContainers.emplace_back(container);
        }
    }

    for (size_t index = 0; index < stoppedContainers.size(); ++index)
    {
        CheckCancelled(cancelEvent);
        const auto& container = stoppedContainers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("remove", name.get(), index + 1, stoppedContainers.size(), "container");
            CheckCancelled(cancelEvent);
        }

        const HRESULT result = container->Delete(WSLCDeleteFlagsNone);
        THROW_IF_FAILED_EXCEPT(result, RPC_E_DISCONNECTED);
    }
}

void ComposeReconciler::CheckCancelled(HANDLE cancelEvent)
{
    THROW_HR_IF_NULL(E_POINTER, cancelEvent);
    const auto waitResult = WaitForSingleObject(cancelEvent, 0);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), waitResult == WAIT_OBJECT_0);
    THROW_HR_IF(E_UNEXPECTED, waitResult != WAIT_TIMEOUT);
}

} // namespace wsl::windows::service::wslc
