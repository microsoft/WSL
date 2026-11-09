// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "ComposeReconciler.h"
#include "WSLCSession.h"

namespace wsl::windows::service::wslc {

ComposeCancellation::ComposeCancellation(HANDLE sessionTerminatingEvent, HANDLE cancelEvent) :
    m_callerProcess(wsl::windows::common::wslutil::OpenCallingProcess(SYNCHRONIZE)), m_handles{sessionTerminatingEvent}
{
    for (const auto handle : {cancelEvent, m_callerProcess.get()})
    {
        if (handle != nullptr)
        {
            m_handles.push_back(handle);
        }
    }
}

void ComposeCancellation::Check() const
{
    const auto result = WaitForMultipleObjects(gsl::narrow<DWORD>(m_handles.size()), m_handles.data(), FALSE, 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), result < WAIT_OBJECT_0 + m_handles.size());
    THROW_HR_IF(E_UNEXPECTED, result != WAIT_TIMEOUT);
}

wil::mutex_release_scope_exit ComposeCancellation::AcquireMutex(HANDLE mutex) const
{
    auto handles = m_handles;
    handles.push_back(mutex);
    const auto index = gsl::narrow<DWORD>(handles.size() - 1);
    const auto result = WaitForMultipleObjects(gsl::narrow<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), result < WAIT_OBJECT_0 + index);
    THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 + index && result != WAIT_ABANDONED_0 + index);

    auto lock = wil::ReleaseMutex_scope_exit(mutex);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ABANDONED_WAIT_0), result == WAIT_ABANDONED_0 + index);
    Check();
    return lock;
}

ComposeReconciler::ComposeReconciler(WSLCSession& session) noexcept : m_session(session)
{
}

ComposeExecutionResult ComposeReconciler::Execute(
    WSLCComposeAction action,
    const std::optional<ComposeSpec>& desiredProject,
    std::string_view projectKey,
    const WSLCComposeActionOptions& actionOptions,
    const ComposeCancellation& cancellation,
    const ComposeProgressReporter& progressReporter,
    std::optional<wsl::windows::common::Error>& primaryError)
{
    cancellation.Check();
    auto projectLockEntry = ResolveProjectLock(projectKey);
    auto projectLock = cancellation.AcquireMutex(projectLockEntry->Lock.get());
    // Kernel mutexes are recursive; nested execution must not interleave project mutations.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), projectLockEntry->Active);
    projectLockEntry->Active = true;
    auto clearActive = wil::scope_exit([&]() { projectLockEntry->Active = false; });
    auto containers = m_session.DiscoverComposeContainers(projectKey);
    cancellation.Check();

    switch (action)
    {
    case WSLCComposeActionCreate:
        THROW_HR_IF(E_INVALIDARG, !desiredProject.has_value());
        // Create reuses a nonempty discovered set as the project's existing resources.
        if (containers.empty())
        {
            containers = Create(*desiredProject, cancellation, progressReporter);
        }
        break;

    case WSLCComposeActionUp:
        THROW_HR_IF(E_INVALIDARG, !desiredProject.has_value());
        containers = Up(*desiredProject, std::move(containers), cancellation, progressReporter);
        break;

    case WSLCComposeActionStart:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Start(containers, cancellation, progressReporter);
        break;

    case WSLCComposeActionAttach:
        // The caller uses the observed container identities to establish attachment.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        break;

    case WSLCComposeActionStop:
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), containers.empty());
        Stop(containers, actionOptions.Value.Stop.TimeoutSeconds, cancellation, progressReporter, primaryError);
        break;

    case WSLCComposeActionValidate:
    default:
        THROW_HR(E_INVALIDARG);
    }

    return {
        .AffectedContainers = ObserveContainers(containers),
    };
}

std::shared_ptr<ComposeReconciler::ProjectLock> ComposeReconciler::ResolveProjectLock(std::string_view projectKey)
{
    std::lock_guard projectLocksLock(m_projectLocksLock);
    // Holders and waiters retain the same entry; map-only entries are idle and reclaimable.
    std::erase_if(m_projectLocks, [](const auto& entry) { return entry.second.use_count() == 1; });

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

        wil::unique_cotaskmem_ansistring image;
        THROW_IF_FAILED(container->GetImage(&image));
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(entry.Image, image.get()) != 0);
        result.emplace_back(entry);
    }

    return result;
}

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Create(
    const ComposeSpec& project, const ComposeCancellation& cancellation, const ComposeProgressReporter& progressReporter)
{
    return m_session.CreateComposeContainers(project, cancellation, progressReporter);
}

std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> ComposeReconciler::Up(
    const ComposeSpec& project,
    std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>> containers,
    const ComposeCancellation& cancellation,
    const ComposeProgressReporter& progressReporter)
{
    // Up removes current containers before creating replacements. CreateComposeContainers deletes the default network,
    // so failures can leave the project partially or fully absent.
    for (size_t index = 0; index < containers.size(); ++index)
    {
        cancellation.Check();
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("remove", name.get(), index + 1, containers.size(), "container");
            cancellation.Check();
        }

        const HRESULT result = container->Delete(WSLCDeleteFlagsForce);
        if (result == RPC_E_DISCONNECTED)
        {
            // A disconnected wrapper retains its terminal state; disconnection alone does not establish deletion.
            WSLCContainerState state{};
            THROW_IF_FAILED(container->GetState(&state));
            THROW_HR_IF(result, state != WslcContainerStateDeleted);
        }
        THROW_IF_FAILED_EXCEPT(result, RPC_E_DISCONNECTED);
    }

    cancellation.Check();
    auto result = m_session.CreateComposeContainers(project, cancellation, progressReporter);
    Start(result, cancellation, progressReporter);
    return result;
}

void ComposeReconciler::Start(const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers, const ComposeCancellation& cancellation, const ComposeProgressReporter& progressReporter)
{
    for (size_t index = 0; index < containers.size(); ++index)
    {
        cancellation.Check();
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("start", name.get(), index + 1, containers.size(), "container");
            cancellation.Check();
        }

        const HRESULT result = container->Start(WSLCContainerStartFlagsNone, nullptr, nullptr);
        // Docker Compose start accepts containers that are already running.
        THROW_IF_FAILED_EXCEPT(result, WSLC_E_CONTAINER_IS_RUNNING);
    }
}

void ComposeReconciler::Stop(
    const std::vector<Microsoft::WRL::ComPtr<IWSLCContainer>>& containers,
    ULONG timeout,
    const ComposeCancellation& cancellation,
    const ComposeProgressReporter& progressReporter,
    std::optional<wsl::windows::common::Error>& primaryError)
{
    THROW_HR_IF(E_INVALIDARG, timeout > LONG_MAX);

    // Attempt the remaining stops while retaining the first container failure.
    HRESULT firstFailure = S_OK;
    for (size_t index = 0; index < containers.size(); ++index)
    {
        cancellation.Check();
        const auto& container = containers[index];
        if (progressReporter)
        {
            wil::unique_cotaskmem_ansistring name;
            THROW_IF_FAILED(container->GetName(&name));
            progressReporter("stop", name.get(), index + 1, containers.size(), "container");
            cancellation.Check();
        }

        const HRESULT result = container->Stop(WSLCSignalNone, static_cast<LONG>(timeout));
        // Docker Compose stop accepts containers that are already stopped.
        if (FAILED(result) && result != WSLC_E_CONTAINER_NOT_RUNNING && SUCCEEDED(firstFailure))
        {
            firstFailure = result;
            const auto* context = wsl::windows::common::ExecutionContext::Current();
            if (context != nullptr && context->ReportedError().has_value() && context->ReportedError()->Code == result)
            {
                // Later stops can update the thread's error context; retain the first failure's details.
                LOG_IF_FAILED(wil::ResultFromException([&]() { primaryError = context->ReportedError(); }));
            }
        }
    }

    THROW_IF_FAILED(firstFailure);
}

} // namespace wsl::windows::service::wslc
