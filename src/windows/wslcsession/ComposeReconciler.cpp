// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeReconciler.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

ComposeReconciler::ComposeReconciler(WSLCSession& Session) noexcept : m_session(Session)
{
}

ComposeExecutionResult ComposeReconciler::Execute(
    WSLCComposeAction Action, const ComposeSpec* DesiredProject, std::string_view ProjectKey, const WSLCComposeActionOptions& ActionOptions, HANDLE CancelEvent)
{
    CheckCancelled(CancelEvent);
    auto projectLockEntry = ResolveProjectLock(ProjectKey);
    std::unique_lock projectLock(projectLockEntry->Lock, std::defer_lock);
    while (!projectLock.try_lock_for(std::chrono::milliseconds{100}))
    {
        CheckCancelled(CancelEvent);
    }
    CheckCancelled(CancelEvent);
    auto containers = m_session.DiscoverComposeContainers(ProjectKey);
    CheckCancelled(CancelEvent);

    switch (Action)
    {
    case WSLCComposeActionCreate:
        THROW_HR_IF_NULL(E_INVALIDARG, DesiredProject);
        if (containers.empty())
        {
            containers = Create(*DesiredProject, CancelEvent);
        }
        break;

    case WSLCComposeActionUp:
        THROW_HR_IF_NULL(E_INVALIDARG, DesiredProject);
        containers = Up(*DesiredProject, std::move(containers), CancelEvent);
        break;

    case WSLCComposeActionStart:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Start(containers, CancelEvent);
        break;

    case WSLCComposeActionAttach:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        break;

    case WSLCComposeActionStop:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Stop(containers, ActionOptions.Value.Stop.TimeoutSeconds, CancelEvent);
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    return {
        .ProjectKey = std::string{ProjectKey},
        .AffectedContainers = ObserveContainers(containers),
    };
}

std::shared_ptr<ComposeReconciler::ProjectLock> ComposeReconciler::ResolveProjectLock(std::string_view ProjectKey)
{
    std::lock_guard projectLocksLock(m_projectLocksLock);
    const auto existing = m_projectLocks.find(std::string{ProjectKey});
    if (existing != m_projectLocks.end())
    {
        return existing->second;
    }

    auto projectLock = std::make_shared<ProjectLock>();
    const auto [entry, inserted] = m_projectLocks.emplace(std::string{ProjectKey}, projectLock);
    WI_ASSERT(inserted);
    return entry->second;
}

std::vector<WSLCContainerEntry> ComposeReconciler::ObserveContainers(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers)
{
    std::vector<WSLCContainerEntry> result;
    result.reserve(Containers.size());
    for (const auto& container : Containers)
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

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Create(const ComposeSpec& Project, HANDLE CancelEvent)
{
    return m_session.CreateComposeContainers(Project, CancelEvent);
}

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Up(
    const ComposeSpec& Project, std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> Containers, HANDLE CancelEvent)
{
    for (const auto& container : Containers)
    {
        CheckCancelled(CancelEvent);
        const HRESULT result = container->Delete(WSLCDeleteFlagsForce);
        THROW_IF_FAILED_EXCEPT(result, RPC_E_DISCONNECTED);
    }

    CheckCancelled(CancelEvent);
    auto result = m_session.CreateComposeContainers(Project, CancelEvent);
    Start(result, CancelEvent);
    return result;
}

void ComposeReconciler::Start(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers, HANDLE CancelEvent)
{
    for (const auto& container : Containers)
    {
        CheckCancelled(CancelEvent);
        const HRESULT result = container->Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
        THROW_IF_FAILED_EXCEPT(result, WSLC_E_CONTAINER_IS_RUNNING);
    }
}

void ComposeReconciler::Stop(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& Containers, ULONG Timeout, HANDLE CancelEvent)
{
    THROW_HR_IF(E_INVALIDARG, Timeout > LONG_MAX);

    HRESULT firstFailure = S_OK;
    for (const auto& container : Containers)
    {
        CheckCancelled(CancelEvent);
        const HRESULT result = container->Stop(WSLCSignalSIGTERM, static_cast<LONG>(Timeout));
        if (FAILED(result) && result != WSLC_E_CONTAINER_NOT_RUNNING && SUCCEEDED(firstFailure))
        {
            firstFailure = result;
        }
    }

    THROW_IF_FAILED(firstFailure);
}

void ComposeReconciler::CheckCancelled(HANDLE CancelEvent)
{
    THROW_HR_IF_NULL(E_POINTER, CancelEvent);
    const auto waitResult = WaitForSingleObject(CancelEvent, 0);
    THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), waitResult == WAIT_OBJECT_0);
    THROW_HR_IF(E_UNEXPECTED, waitResult != WAIT_TIMEOUT);
}

} // namespace wsl::windows::service::wslc
