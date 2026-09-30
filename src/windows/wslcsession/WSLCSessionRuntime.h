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
        // Invoked while tearing down the VM, with the VM-scoped state still alive. The argument is
        // true only for a permanent session shutdown (not an idle teardown): on idle teardown the
        // container wrappers must be kept alive so client COM references stay valid and are reused
        // when the VM restarts.
        std::function<void(bool permanent)> TearDownSessionState;
        std::function<void()> OnSpontaneousExit;
        WSLCVirtualMachine::TOnCrashDump OnCrashDump;

        // Fired when a VM has started (best-effort). Invoked without the runtime lock held so the
        // handler may call back into the session (e.g. to run setup in the VM). Fired every time a
        // VM is (re)created for the session.
        std::function<void()> OnVmStarted;

        // Fired when a VM is about to be torn down (best-effort), while the VM is still alive. Invoked
        // without the runtime lock held so the handler may call back into the session or into a plugin
        // that acquires a VM lease. Fires exactly once per OnVmStarted -- on both idle and permanent
        // teardown -- and the teardown always follows: once this is raised the VM is committed to
        // stopping, and any lease that is not part of this callback waits for the next VM. On a
        // permanent teardown the session is terminating, so a callback that resolves this session
        // (e.g. to create a process) fails cleanly instead of restarting the VM.
        std::function<void()> OnVmStopping;
    };

    struct SessionContext
    {
        ULONG Id{};
        std::wstring DisplayName;
        const std::atomic<bool>* Terminating{};
        wil::shared_event SessionTerminatingEvent;
        wil::shared_event SessionTerminatedEvent;
    };

    // Whether a lease may bring a VM up, or must be served by whatever VM is already running.
    //
    // Acquire is correct for every ordinary caller: it starts the VM if there is none, and because an
    // announced stop always happens, a VM with one pending is unusable even though it is still
    // running -- the lease waits for the teardown and is then served by a fresh VM.
    //
    // ExistingOnly is for plugins. A plugin call is a side effect of the session's own activity, never
    // a reason to create a VM, so it neither starts one nor waits for a teardown: it is served by the
    // running VM, including one committed to stopping, and fails with WSLC_E_VM_NOT_RUNNING when there
    // is none. Waiting is not an option for the calls that matter -- a plugin reentering from its
    // OnWslcVmStopping handler is the reason the teardown is blocked, so it would deadlock against
    // itself.
    enum class VmLeasePolicy
    {
        Acquire,
        ExistingOnly,
    };

    class VmLease
    {
    public:
        VmLease() = default;
        explicit VmLease(WSLCSessionRuntime& Runtime, VmLeasePolicy Policy = VmLeasePolicy::Acquire);
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
        explicit LockedRuntime(WSLCSessionRuntime& Runtime, VmLeasePolicy Policy = VmLeasePolicy::Acquire);

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
    [[nodiscard]] wil::rwlock_release_exclusive_scope_exit TryLockExclusive() noexcept;
    IdleState& Idle() noexcept;
    std::shared_ptr<IdleState> IdleStateShared() const noexcept;
    VmState State() const noexcept;
    VmExitDisposition ExitDisposition() const noexcept;
    bool VmExited() const noexcept;
    void ResetDockerdReady() noexcept;
    void OnProcessLog(const gsl::span<char>& buffer, PCSTR source) noexcept;
    void SetContainerdProcess(ServiceRunningProcess&& process);
    void SetDockerdProcess(ServiceRunningProcess&& process);

    void SetSwapVhdPath(std::filesystem::path path);
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
    [[nodiscard]] VmLease AcquireVmLease(VmLeasePolicy Policy = VmLeasePolicy::Acquire);
    [[nodiscard]] LockedRuntime Acquire(VmLeasePolicy Policy = VmLeasePolicy::Acquire);

    [[nodiscard]] bool TriggerIdleTerminationForTest();

    // runtimeLock is an exclusive hold on m_lock (this runtime's lock), which is dropped and reacquired
    // internally so the OnVmStopping notification can fire without it and TearDownVmLockHeld runs with it.
    void Shutdown(wil::rwlock_release_exclusive_scope_exit& runtimeLock, WSLCVirtualMachineTerminationReason& terminationReason, std::wstring& terminationDetails);

private:
    bool IdleTerminationEnabled() const noexcept;
    int StopProcess(ServiceRunningProcess& Process, DWORD TerminateTimeoutMs, DWORD KillTimeoutMs);

    // Fires the OnVmStarted hook for the VM instance identified by 'Generation'. Must be called
    // without the runtime lock held (the handler may call back into the session), with an activity
    // reference held so idle teardown cannot race the VM down before the notification is delivered.
    void NotifyVmStarted(uint64_t Generation);

    // Fires the OnVmStopping hook, if OnVmStarted was delivered for 'Generation' and that instance is
    // still the one running. Must be called without the runtime lock held: the handler may call into a
    // plugin that acquires a VM lease (which takes m_lock), so firing under the lock would deadlock.
    // Called while the VM is still running so the handler can still operate on it, and only after
    // BeginVmStopLockHeld has committed the VM to stopping, so the announcement is always true.
    void NotifyVmStopping(uint64_t Generation);

    // Commits the current VM to stopping. Ordinary leases arriving from here until EndVmStop() release
    // the shared lock and wait for the teardown rather than being served by a VM that is going away.
    _Requires_exclusive_lock_held_(m_lock)
    void BeginVmStopLockHeld() noexcept;

    // Releases waiting leases, which then start (or wait for) the next VM. Safe to call when no stop
    // is pending, so it can be run unconditionally from a scope_exit.
    void EndVmStop() noexcept;

    WSLCSession* m_session{};
    RuntimeHooks m_hooks;

    ULONG m_id{};
    std::wstring m_displayName;
    bool m_initialized{};
    const std::atomic<bool>* m_terminating{};
    wil::shared_event m_sessionTerminatingEvent;
    wil::shared_event m_sessionTerminatedEvent;

    DWORD m_vmFactoryGitCookie{};
    wil::com_ptr<IGlobalInterfaceTable> m_git;
    const WSLCSessionInitSettings* m_settings{};

    std::optional<WSLCVirtualMachine> m_virtualMachine;
    // Lock-free mirror of m_virtualMachine.has_value(): written under the runtime lock immediately
    // after the VM object is constructed and immediately before it is destroyed, read without the
    // lock by container teardown (~WSLCContainerImpl runs without a VM lease). Reading the optional
    // itself there would race with the reset() performed by an idle teardown on another thread.
    std::atomic<bool> m_hasVm{false};
    std::optional<IORelay> m_ioRelay;
    std::optional<DockerEventTracker> m_eventTracker;
    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLCVolumes> m_volumes;
    std::optional<ServiceRunningProcess> m_containerdProcess;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    wil::unique_event m_vmExitedEvent;
    // Lock-free mirror of "the current VM instance has exited": written under the runtime lock when a
    // VM starts (false) or is observed dead during teardown (true), read without the lock by container
    // teardown to skip VM-dependent cleanup. Avoids racing on m_vmExitedEvent, which is reset/replaced
    // under the lock.
    std::atomic<bool> m_vmExited{false};
    wil::unique_event m_dockerdReadyEvent{wil::EventOptions::ManualReset};

    std::filesystem::path m_swapVhdPath;
    bool m_storageMounted{false};
    std::mutex m_allocatedPortsLock;
    std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>> m_allocatedPorts;

    wil::srwlock m_lock;
    std::atomic<VmState> m_vmState{VmState::None};
    std::atomic<VmExitDisposition> m_vmExitDisposition{VmExitDisposition::Active};

    // Identifies the current VM instance. Bumped under m_lock by StartVmLockHeld, so a notification
    // whose delivery had to drop the runtime lock can still tell whether it is describing the VM it
    // was raised for, or one that has since been torn down and replaced.
    std::atomic<uint64_t> m_vmGeneration{0};

    // The VM instance whose OnVmStarted has been delivered and not yet retired (0 = none). Set by
    // NotifyVmStarted, retired by NotifyVmStopping as it announces the stop, and also cleared by
    // TearDownVmLockHeld to cover a VM that goes away without a stop ever being announced. Retiring
    // it on the announcement is what makes OnVmStopping exactly-once: the stop is committed before it
    // is announced, so the instance can never come back and a second stop for the same generation
    // (Terminate() racing an idle teardown) finds nothing to retire and stays silent.
    //
    // The generation check and the hook invocation both happen under m_notifyLock, so a start and a
    // stop racing on different threads cannot interleave (which would otherwise let a stale
    // OnVmStarted be delivered after the OnVmStopping it should have preceded), and a notification
    // that lost such a race is dropped rather than misattributed to whichever VM is running by the
    // time it is delivered. Recursive because the handler may reentrantly restart the VM, re-entering
    // these notifications on the same thread.
    std::recursive_mutex m_notifyLock;
    std::atomic<uint64_t> m_notifiedGeneration{0};

    // Published under m_lock once a stop is decided and cleared when the teardown has finished. While
    // it is set the VM is going away no matter what, so an ordinary lease must not be served by it;
    // it waits on m_vmStopCompleteEvent (holding no lock, or the teardown could never reacquire the
    // exclusive lock) and is then served by the next VM. Manual-reset and initially signaled so a
    // lease that never sees a stop pending never blocks.
    std::atomic<bool> m_vmStopPending{false};
    wil::unique_event m_vmStopCompleteEvent{wil::EventOptions::ManualReset | wil::EventOptions::Signaled};

    std::shared_ptr<IdleState> m_idleState{std::make_shared<IdleState>()};

    WSLCVirtualMachineTerminationReason m_lastTerminationReason{WSLCVirtualMachineTerminationReasonUnknown};
    std::wstring m_lastTerminationDetails;
};

} // namespace wsl::windows::service::wslc
