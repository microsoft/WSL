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
    auto project = ResolveProject(Action, DesiredProject, ProjectKey);
    std::unique_lock projectLock(project->Lock, std::defer_lock);
    while (!projectLock.try_lock_for(std::chrono::milliseconds{100}))
    {
        CheckCancelled(CancelEvent);
    }
    CheckCancelled(CancelEvent);

    switch (Action)
    {
    case WSLCComposeActionCreate:
        if (project->Containers.empty())
        {
            project->Spec = *DesiredProject;
            Create(*project, CancelEvent);
        }
        break;

    case WSLCComposeActionUp:
        project->Spec = *DesiredProject;
        Up(*project, CancelEvent);
        break;

    case WSLCComposeActionStart:
        Start(*project, CancelEvent);
        break;

    case WSLCComposeActionAttach:
        break;

    case WSLCComposeActionStop:
        Stop(*project, ActionOptions.Value.Stop.TimeoutSeconds, CancelEvent);
        break;

    default:
        THROW_HR(E_INVALIDARG);
    }

    CheckCancelled(CancelEvent);
    return {
        .ProjectKey = std::string{ProjectKey},
        .AffectedContainers = ObserveContainers(*project),
    };
}

std::shared_ptr<ComposeReconciler::ProjectState> ComposeReconciler::ResolveProject(
    WSLCComposeAction Action, const ComposeSpec* DesiredProject, std::string_view ProjectKey)
{
    std::lock_guard projectsLock(m_projectsLock);
    const auto existing = m_projects.find(std::string{ProjectKey});
    if (existing != m_projects.end())
    {
        return existing->second;
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Action != WSLCComposeActionCreate && Action != WSLCComposeActionUp);
    THROW_HR_IF_NULL(E_INVALIDARG, DesiredProject);

    auto state = std::make_shared<ProjectState>();
    state->Spec = *DesiredProject;
    const auto [entry, inserted] = m_projects.emplace(std::string{ProjectKey}, state);
    WI_ASSERT(inserted);
    return entry->second;
}

std::vector<WSLCContainerEntry> ComposeReconciler::ObserveContainers(const ProjectState& Project)
{
    std::vector<WSLCContainerEntry> result;
    result.reserve(Project.Containers.size());
    for (const auto& container : Project.Containers)
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

void ComposeReconciler::Create(ProjectState& Project, HANDLE CancelEvent)
{
    Project.Containers = m_session.CreateComposeContainers(Project.Spec, CancelEvent);
}

void ComposeReconciler::Up(ProjectState& Project, HANDLE CancelEvent)
{
    for (const auto& container : Project.Containers)
    {
        CheckCancelled(CancelEvent);
        const HRESULT result = container->Delete(WSLCDeleteFlagsForce);
        THROW_IF_FAILED_EXCEPT(result, RPC_E_DISCONNECTED);
    }

    CheckCancelled(CancelEvent);
    Project.Containers = m_session.CreateComposeContainers(Project.Spec, CancelEvent);
    Start(Project, CancelEvent);
}

void ComposeReconciler::Start(ProjectState& Project, HANDLE CancelEvent)
{
    for (const auto& container : Project.Containers)
    {
        CheckCancelled(CancelEvent);
        const HRESULT result = container->Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
        THROW_IF_FAILED_EXCEPT(result, WSLC_E_CONTAINER_IS_RUNNING);
    }
}

void ComposeReconciler::Stop(ProjectState& Project, ULONG Timeout, HANDLE CancelEvent)
{
    THROW_HR_IF(E_INVALIDARG, Timeout > LONG_MAX);

    HRESULT firstFailure = S_OK;
    for (const auto& container : Project.Containers)
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
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), WaitForSingleObject(CancelEvent, 0) == WAIT_OBJECT_0);
}

} // namespace wsl::windows::service::wslc
