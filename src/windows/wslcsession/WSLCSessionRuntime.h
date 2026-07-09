/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionRuntime.h

Abstract:

    Contains the definition for WSLCSessionRuntime.

--*/

#pragma once

#include "wslc.h"
#include "WSLCVirtualMachine.h"
#include "WSLCVolumes.h"
#include "WSLCIdleState.h"
#include "DockerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"
#include "ServiceProcessLauncher.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wsl::windows::service::wslc {

class WSLCSession;

class WSLCSessionRuntime
{
public:
    enum class VmState
    {
        None,
        Starting,
        Running,
        Stopping,
    };

    enum class VmExitDisposition
    {
        Active,
        StopRequested,
        ExitClaimed,
    };

    struct RuntimeHooks
    {
        std::function<void()> BringUp;
        std::function<void()> RecoverState;
        std::function<void()> TearDownSessionState;
        std::function<void()> OnSpontaneousExit;
        WSLCVirtualMachine::TOnCrashDump OnCrashDump;

        // Fired when a VM has started (best-effort). Invoked without the runtime lock held so the
        // handler may call back into the session (e.g. to run setup in the VM). Fired every time a
        // VM is (re)created for the session.
        std::function<void()> OnVmStarted;

        // Fired when a VM is about to be torn down (best-effort). Invoked under the runtime lock, so
        // the handler must not call back into the session. Paired with a prior OnVmStarted.
        std::function<void()> OnVmStopping;
    };

    struct SessionContext
    {
        ULONG Id{};
        std::wstring DisplayName;
        const std::atomic<bool>* Terminating{};
        wil::unique_event* SessionTerminatingEvent{};
        wil::unique_event* SessionTerminatedEvent{};
    };

    class VmLease
    {
    public:
        VmLease() = default;
        explicit VmLease(WSLCSessionRuntime& Runtime);
        VmLease(VmLease&& Other) noexcept;
        VmLease& operator=(VmLease&& Other) noexcept;
        ~VmLease();

        VmLease(const VmLease&) = delete;
        VmLease& operator=(const VmLease&) = delete;

    private:
        WSLCSessionRuntime* m_runtime{};
        wil::rwlock_release_shared_scope_exit m_lock;
    };

    class LockedRuntime
    {
    public:
        LockedRuntime() = default;
        explicit LockedRuntime(WSLCSessionRuntime& Runtime);

        WSLCVirtualMachine& Vm();
        IORelay* Relay();
        DockerHTTPClient& Docker();

    private:
        WSLCSessionRuntime* m_runtime{};
        VmLease m_lease;
    };

    explicit WSLCSessionRuntime(WSLCSession& Session) noexcept;

    void Initialize(
        DWORD vmFactoryGitCookie,
        wil::com_ptr<IGlobalInterfaceTable> git,
        const WSLCSessionInitSettings* settings,
        std::chrono::milliseconds idleGrace,
        SessionContext sessionContext,
        RuntimeHooks hooks);

    WSLCVirtualMachine& Vm();
    bool HasVm() const noexcept;
    IORelay* Relay();
    bool HasRelay() const noexcept;
    DockerHTTPClient& Docker();
    bool HasDocker() const noexcept;
    DockerEventTracker& Events();
    bool HasEvents() const noexcept;
    WSLCVolumes& Volumes();
    bool HasVolumes() const noexcept;
    wil::srwlock& Lock() noexcept;
    IdleState& Idle() noexcept;
    std::shared_ptr<IdleState> IdleStateShared() const noexcept;
    VmState State() const noexcept;
    VmExitDisposition ExitDisposition() const noexcept;
    std::atomic<VmState>& StateAtomic() noexcept;
    std::atomic<VmExitDisposition>& ExitDispositionAtomic() noexcept;
    wil::unique_event& VmExitedEvent() noexcept;
    wil::unique_event& DockerdReadyEvent() noexcept;
    std::optional<ServiceRunningProcess>& ContainerdProcess();
    std::optional<ServiceRunningProcess>& DockerdProcess();

    std::filesystem::path& SwapVhdPath() noexcept;
    void SetStorageMounted(bool value) noexcept;

    std::mutex& AllocatedPortsLock() noexcept;
    std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>>& AllocatedPorts() noexcept;

    [[nodiscard]] bool TryClaimExpectedStop() noexcept;
    [[nodiscard]] bool TryClaimSpontaneousExit() noexcept;

    _Requires_exclusive_lock_held_(m_lock)
    void StartVmLockHeld();
    _Requires_exclusive_lock_held_(m_lock)
    void StopVmLockHeld();
    _Requires_exclusive_lock_held_(m_lock)
    void TearDownVmLockHeld(bool CaptureTerminationReason = false);
    void EnsureVmRunning();
    void OnIdleTimer();
    void OnVmExited();
    void InitializeDockerRuntime(const std::filesystem::path& storagePath);
    [[nodiscard]] VmLease AcquireVmLease();
    [[nodiscard]] LockedRuntime Acquire();

    [[nodiscard]] bool TriggerIdleTerminationForTest();

    void Shutdown(wil::rwlock_release_exclusive_scope_exit& sessionLock, WSLCVirtualMachineTerminationReason& terminationReason, std::wstring& terminationDetails);

private:
    bool IdleTerminationEnabled() const noexcept;
    int StopProcess(ServiceRunningProcess& Process, DWORD TerminateTimeoutMs, DWORD KillTimeoutMs);

    // Fires the OnVmStarted hook. Must be called without the runtime lock held (the handler may
    // call back into the session), with an activity reference held so idle teardown cannot race the
    // VM down before the notification is delivered.
    void NotifyVmStarted();

    WSLCSession* m_session{};
    RuntimeHooks m_hooks;

    ULONG m_id{};
    std::wstring m_displayName;
    bool m_initialized{};
    const std::atomic<bool>* m_terminating{};
    wil::unique_event* m_sessionTerminatingEvent{};
    wil::unique_event* m_sessionTerminatedEvent{};

    DWORD m_vmFactoryGitCookie{};
    wil::com_ptr<IGlobalInterfaceTable> m_git;
    const WSLCSessionInitSettings* m_settings{};

    std::optional<WSLCVirtualMachine> m_virtualMachine;
    std::optional<IORelay> m_ioRelay;
    std::optional<DockerEventTracker> m_eventTracker;
    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLCVolumes> m_volumes;
    std::optional<ServiceRunningProcess> m_containerdProcess;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    wil::unique_event m_vmExitedEvent;
    wil::unique_event m_dockerdReadyEvent{wil::EventOptions::ManualReset};

    std::filesystem::path m_swapVhdPath;
    bool m_storageMounted{false};
    std::mutex m_allocatedPortsLock;
    std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>> m_allocatedPorts;

    wil::srwlock m_lock;
    std::atomic<VmState> m_vmState{VmState::None};
    std::atomic<VmExitDisposition> m_vmExitDisposition{VmExitDisposition::Active};

    // Set when OnVmStarted has fired for the current VM; gates the paired OnVmStopping so a VM that
    // never finished starting (or had no started notification) does not emit a spurious stopping.
    std::atomic<bool> m_vmStartNotified{false};
    std::shared_ptr<IdleState> m_idleState{std::make_shared<IdleState>()};

    WSLCVirtualMachineTerminationReason m_lastTerminationReason{WSLCVirtualMachineTerminationReasonUnknown};
    std::wstring m_lastTerminationDetails;
};

} // namespace wsl::windows::service::wslc
