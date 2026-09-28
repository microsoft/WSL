/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionRuntime.cpp

Abstract:

    Contains the implementation for WSLCSessionRuntime.

--*/

#include "precomp.h"
#include "WSLCSessionRuntime.h"
#include "WSLCSession.h"
#include "WSLCSessionDefaults.h"

using wsl::windows::service::wslc::WSLCSessionRuntime;

namespace {

constexpr auto c_dockerdReadyLogLine = "API listen on /var/run/docker.sock";

// How long a VM lease waits for an announced stop to complete before logging that it is still
// blocked. Purely diagnostic: the wait itself is unbounded (see VmLease).
constexpr DWORD c_vmStopWaitLogIntervalMs = 30 * 1000;

} // namespace

namespace wsl::windows::service::wslc {

WSLCSessionRuntime::WSLCSessionRuntime(WSLCSession& Session) noexcept : m_session(&Session)
{
}

void WSLCSessionRuntime::Initialize(
    DWORD vmFactoryGitCookie,
    wil::com_ptr<IGlobalInterfaceTable> git,
    const WSLCSessionInitSettings* settings,
    std::chrono::milliseconds idleGrace,
    SessionContext sessionContext,
    RuntimeHooks hooks)
{
    m_vmFactoryGitCookie = vmFactoryGitCookie;
    m_git = std::move(git);
    m_settings = settings;
    m_id = sessionContext.Id;
    m_displayName = std::move(sessionContext.DisplayName);
    m_terminating = sessionContext.Terminating;
    m_sessionTerminatingEvent = sessionContext.SessionTerminatingEvent;
    m_sessionTerminatedEvent = sessionContext.SessionTerminatedEvent;
    m_hooks = std::move(hooks);

    m_idleState->Initialize(idleGrace, [this]() { OnIdleTimer(); });

    // Session-scoped: subscriptions must survive VM restarts. Rebound per-VM in InitializeDockerRuntime.
    m_eventTracker.emplace(*m_session);

    m_initialized = true;
}

WSLCVirtualMachine& WSLCSessionRuntime::Vm()
{
    WI_ASSERT(m_virtualMachine.has_value());
    return m_virtualMachine.value();
}

bool WSLCSessionRuntime::HasVm() const noexcept
{
    return m_hasVm.load();
}

IORelay* WSLCSessionRuntime::Relay()
{
    return m_ioRelay ? &m_ioRelay.value() : nullptr;
}

bool WSLCSessionRuntime::HasRelay() const noexcept
{
    return m_ioRelay.has_value();
}

DockerHTTPClient& WSLCSessionRuntime::Docker()
{
    WI_ASSERT(m_dockerClient.has_value());
    return m_dockerClient.value();
}

bool WSLCSessionRuntime::HasDocker() const noexcept
{
    return m_dockerClient.has_value();
}

DockerEventTracker& WSLCSessionRuntime::Events()
{
    WI_ASSERT(m_eventTracker.has_value());
    return m_eventTracker.value();
}

bool WSLCSessionRuntime::HasEvents() const noexcept
{
    return m_eventTracker.has_value();
}

WSLCVolumes& WSLCSessionRuntime::Volumes()
{
    WI_ASSERT(m_volumes.has_value());
    return m_volumes.value();
}

bool WSLCSessionRuntime::HasVolumes() const noexcept
{
    return m_volumes.has_value();
}

wil::rwlock_release_exclusive_scope_exit WSLCSessionRuntime::TryLockExclusive() noexcept
{
    return m_lock.try_lock_exclusive();
}

IdleState& WSLCSessionRuntime::Idle() noexcept
{
    return *m_idleState;
}

std::shared_ptr<IdleState> WSLCSessionRuntime::IdleStateShared() const noexcept
{
    return m_idleState;
}

WSLCSessionRuntime::VmState WSLCSessionRuntime::State() const noexcept
{
    return m_vmState.load();
}

WSLCSessionRuntime::VmExitDisposition WSLCSessionRuntime::ExitDisposition() const noexcept
{
    return m_vmExitDisposition.load();
}

bool WSLCSessionRuntime::VmExited() const noexcept
{
    return m_vmExited.load();
}

void WSLCSessionRuntime::ResetDockerdReady() noexcept
{
    m_dockerdReadyEvent.ResetEvent();
}

void WSLCSessionRuntime::OnProcessLog(const gsl::span<char>& buffer, PCSTR source) noexcept
try
{
    if (buffer.empty())
    {
        return;
    }

    std::string entry{buffer.begin(), buffer.end()};
    WSL_LOG(
        "ContainerdLog",
        TraceLoggingValue(source, "Source"),
        TraceLoggingValue(entry.c_str(), "Content"),
        TraceLoggingValue(m_displayName.c_str(), "Name"));

    if (!m_dockerdReadyEvent.is_signaled() && entry.find(c_dockerdReadyLogLine) != std::string::npos)
    {
        m_dockerdReadyEvent.SetEvent();
    }
}
CATCH_LOG()

void WSLCSessionRuntime::SetContainerdProcess(ServiceRunningProcess&& process)
{
    m_containerdProcess = std::move(process);
}

void WSLCSessionRuntime::SetDockerdProcess(ServiceRunningProcess&& process)
{
    m_dockerdProcess = std::move(process);
}

void WSLCSessionRuntime::SetSwapVhdPath(std::filesystem::path path)
{
    m_swapVhdPath = std::move(path);
}

void WSLCSessionRuntime::SetStorageMounted(bool value) noexcept
{
    m_storageMounted = value;
}

std::mutex& WSLCSessionRuntime::AllocatedPortsLock() noexcept
{
    return m_allocatedPortsLock;
}

std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>>& WSLCSessionRuntime::AllocatedPorts() noexcept
{
    return m_allocatedPorts;
}

bool WSLCSessionRuntime::IdleTerminationEnabled() const noexcept
{
    // Only tear the VM down when there is persistent storage to recover from. A tmpfs-backed
    // session would lose all image/container state on teardown, so its VM is kept alive once started.
    return m_settings->StoragePath != nullptr;
}

int WSLCSessionRuntime::StopProcess(ServiceRunningProcess& Process, DWORD TerminateTimeoutMs, DWORD KillTimeoutMs)
{
    auto signalResult = Process.Get().Signal(WSLCSignalSIGTERM);
    if (FAILED(signalResult))
    {
        LOG_HR_MSG(signalResult, "Failed to terminate process %i", Process.Get().GetPid());
        return -1;
    }

    try
    {
        return Process.Wait(TerminateTimeoutMs);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        try
        {
            LOG_IF_FAILED(Process.Get().Signal(WSLCSignalSIGKILL));
            return Process.Wait(KillTimeoutMs);
        }
        CATCH_LOG();
    }

    return -1;
}

void WSLCSessionRuntime::EnsureVmRunning()
{
    // Reject leases once the session is terminating/terminated, including on the running fast path:
    // Shutdown() drops the lock to fire OnVmStopping, and a reentrant lease that finds the VM still
    // Running must fail here rather than run work against a VM being permanently torn down.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_terminating->load() || m_sessionTerminatedEvent.is_signaled());

    if (m_vmState.load() == VmState::Running)
    {
        return;
    }

    bool started = false;
    uint64_t generation = 0;
    {
        auto lock = m_lock.lock_exclusive();

        // Re-check under the lock: terminating may have been set since the check above. This also
        // bounds VmLease's retry loop: a lease that races with Terminate() fails here instead of
        // restarting a VM that is being permanently torn down.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_terminating->load() || m_sessionTerminatedEvent.is_signaled());

        if (m_vmState.load() != VmState::Running)
        {
            StartVmLockHeld();
            started = true;
        }

        generation = m_vmGeneration.load();
    }

    // Notify plugins that a VM has started, outside the exclusive lock: the handler forwards to the
    // plugin, which may call back into the session (e.g. WSLCCreateProcess acquires a VM lease and
    // the exclusive lock), so firing under the lock would deadlock. EnsureVmRunning's only caller
    // (VmLease) holds an activity reference across this call, so idle teardown cannot race the VM
    // down in the gap between releasing the lock and notifying.
    if (started)
    {
        NotifyVmStarted(generation);
    }
}

void WSLCSessionRuntime::NotifyVmStarted(uint64_t Generation)
{
    // Hold m_notifyLock across both the pairing-state flip and the hook invocation, so a concurrent
    // NotifyVmStopping on another thread cannot deliver its OnVmStopping in between and observe this
    // OnVmStarted arrive late (which would otherwise leave the plugin with a trailing "started" for a
    // VM that already stopped). m_notifyLock is recursive: the handler may reentrantly restart the VM
    // (e.g. WSLCCreateProcess -> NotifyVmStarted/Stopping) on this same thread, which would self-deadlock
    // under a plain mutex. Suppressed once terminating, else a racing Shutdown leaves OnVmStarted unpaired.
    // m_notifiedGeneration tracks the VM lifecycle, not whether OnVmStarted is installed, so a hooks
    // user that sets only OnVmStopping still gets paired stop notifications.
    auto lock = std::lock_guard(m_notifyLock);

    // Drop the notification if this instance is already gone: between releasing the runtime lock and
    // getting here it may have been torn down and replaced, and announcing it now would misattribute
    // the start to whichever instance is running.
    if (m_terminating->load() || m_vmGeneration.load() != Generation)
    {
        return;
    }

    m_notifiedGeneration.store(Generation);
    if (m_hooks.OnVmStarted)
    {
        m_hooks.OnVmStarted();
    }
}

void WSLCSessionRuntime::NotifyVmStopping(uint64_t Generation)
{
    // See NotifyVmStarted: hold m_notifyLock across both the pairing check and the hook invocation, so
    // the two notifications cannot interleave across threads. Recursive because the handler may
    // reentrantly restart the VM and call NotifyVmStarted on this same thread.
    //
    // N.B. The CAS below retires the generation as the stop is announced, so a second stop for the
    // same VM (e.g. Terminate() racing an idle teardown that has dropped the lock across its
    // notification) cannot deliver OnVmStopping twice. This is safe because the stop is committed
    // before it is announced: the VM cannot come back, so nothing needs the generation afterwards.
    //
    // Generation 0 is the sentinel for "no VM has been announced": both counters start there, so a
    // session that is torn down without ever starting a VM must not match, or Shutdown would deliver
    // an OnVmStopping that no OnVmStarted ever paired with.
    auto lock = std::lock_guard(m_notifyLock);

    auto expected = Generation;
    if (Generation != 0 && m_notifiedGeneration.compare_exchange_strong(expected, 0) && m_hooks.OnVmStopping)
    {
        m_hooks.OnVmStopping();
    }
}

void WSLCSessionRuntime::BeginVmStopLockHeld() noexcept
{
    // Reset before publishing the flag, so a lease cannot observe the pending stop while the event is
    // still signaled from the previous teardown.
    m_vmStopCompleteEvent.ResetEvent();
    m_vmStopPending.store(true);
}

void WSLCSessionRuntime::EndVmStop() noexcept
{
    m_vmStopPending.store(false);
    m_vmStopCompleteEvent.SetEvent();
}

bool WSLCSessionRuntime::TryClaimExpectedStop() noexcept
{
    auto expected = VmExitDisposition::Active;
    return m_vmExitDisposition.compare_exchange_strong(expected, VmExitDisposition::StopRequested);
}

bool WSLCSessionRuntime::TryClaimSpontaneousExit() noexcept
{
    auto expected = VmExitDisposition::Active;
    return m_vmExitDisposition.compare_exchange_strong(expected, VmExitDisposition::ExitClaimed);
}

void WSLCSessionRuntime::StartVmLockHeld()
{
    WI_ASSERT(m_vmState.load() != VmState::Running);

    WSL_LOG("WslcVmStarting", TraceLoggingValue(m_id, "SessionId"));

    m_vmState.store(VmState::Starting);
    m_vmExitDisposition.store(VmExitDisposition::Active);

    // Identify this instance. Bumped under the runtime lock so a notification that had to drop the
    // lock to fire can tell whether it still describes the VM it was raised for. The first VM is
    // generation 1, so the 0 stored by TearDownVmLockHeld never collides with a live instance.
    m_vmGeneration.fetch_add(1);

    // Tear back down if bring-up fails partway. The VM may have exited on its own during bring-up,
    // so claim the stop first and only tear down if we win it (TryClaimExpectedStop()); otherwise
    // OnVmExited() owns the teardown and we just release the lock to let its Terminate() finish.
    auto startCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (TryClaimExpectedStop())
        {
            TearDownVmLockHeld();
            m_vmState.store(VmState::None);
        }
        else
        {
            WSL_LOG("WslcVmExitedDuringStart", TraceLoggingValue(m_id, "SessionId"));
        }
    });

    // Create a fresh IO relay for this VM instance. The previous one (if any) was stopped
    // during teardown and cannot be restarted.
    m_ioRelay.emplace();

    // Create the VM via the factory. Re-fetch the factory from the GIT so we call it through a
    // proxy marshalled into this thread's apartment (see m_git). The VM produces crash events;
    // the session multiplexes them out to any registered ICrashDumpCallback subscribers via
    // OnCrashDumpWritten.
    wil::com_ptr<IWSLCVirtualMachineFactory> vmFactory;
    THROW_IF_FAILED(m_git->GetInterfaceFromGlobal(m_vmFactoryGitCookie, __uuidof(IWSLCVirtualMachineFactory), vmFactory.put_void()));

    wil::com_ptr<IWSLCVirtualMachine> vm;
    THROW_IF_FAILED(vmFactory->CreateVirtualMachine(&vm));

    m_virtualMachine.emplace(vm.get(), m_settings, m_sessionTerminatingEvent.get(), WSLCVirtualMachine::TOnCrashDump(m_hooks.OnCrashDump));

    // Publish only once the object is constructed. If Initialize() below throws, startCleanup tears
    // the VM down and clears this again.
    m_hasVm.store(true);

    m_virtualMachine->Initialize();

    // Get an event from the service that is signaled when the VM exits.
    m_vmExitedEvent.reset();
    THROW_IF_FAILED(vm->GetTerminationEvent(&m_vmExitedEvent));
    m_vmExited.store(false);

    if (m_hooks.BringUp)
    {
        m_hooks.BringUp();
    }

    // Monitor for unexpected VM exit.
    m_ioRelay->AddHandle(
        std::make_unique<windows::common::io::EventHandle>(m_vmExitedEvent.get(), std::bind(&WSLCSessionRuntime::OnVmExited, this)));

    if (m_hooks.RecoverState)
    {
        m_hooks.RecoverState();
    }

    m_vmState.store(VmState::Running);
    startCleanup.release();

    WSL_LOG("WslcVmStarted", TraceLoggingValue(m_id, "SessionId"));
}

void WSLCSessionRuntime::InitializeDockerRuntime(const std::filesystem::path& storagePath)
{
    // Wait for dockerd to be ready before starting the event tracker.
    THROW_WIN32_IF_MSG(
        ERROR_TIMEOUT, !m_dockerdReadyEvent.wait(m_settings->BootTimeoutMs), "Timed out waiting for dockerd to start");

    [[maybe_unused]] auto [pid, ptyMaster, channel] = m_virtualMachine->Fork(WSLC_FORK::Thread);

    m_dockerClient.emplace(std::move(channel), m_virtualMachine->TerminatingEvent(), m_virtualMachine->VmId(), 10 * 1000);

    // (Re)bind the session-scoped event tracker to this VM's docker client and relay. Existing
    // container subscriptions are preserved across restarts.
    m_eventTracker->Connect(m_dockerClient.value(), *m_ioRelay);

    m_volumes.emplace(m_dockerClient.value(), m_virtualMachine.value(), m_eventTracker.value(), storagePath);
}

void WSLCSessionRuntime::StopVmLockHeld()
{
    if (m_vmState.load() != VmState::Running)
    {
        return;
    }

    WSL_LOG("WslcVmIdleStop", TraceLoggingValue(m_id, "SessionId"));

    // N.B. The caller has claimed StopRequested (via TryClaimExpectedStop), so VM/dockerd/containerd
    // exit callbacks firing from the relay thread during teardown are treated as expected, not as a
    // crash.
    m_vmState.store(VmState::Stopping);

    TearDownVmLockHeld();

    m_vmState.store(VmState::None);
}

void WSLCSessionRuntime::TearDownVmLockHeld(bool CaptureTerminationReason)
{
    // The VM is committed to going away from here on, so the plugin's view of it ends here. Retiring
    // the notified instance up front, before any step below can throw, also covers the case where the
    // VM is torn down without an OnVmStopping ever being announced (e.g. bring-up failed). Written
    // under the runtime lock; NotifyVmStarted/NotifyVmStopping read it under m_notifyLock without the
    // runtime lock, so this store can land while one of them is inside a plugin handler. That is
    // benign: both compare against a generation they captured earlier, so a store of 0 can only make
    // them decline to notify, and by this point either the stop has already been announced (its CAS
    // ran first) or the instance is gone and must not be announced at all.
    m_notifiedGeneration.store(0);

    // Latch whether the guest is already dead before running session-state cleanup so container
    // teardown (ReleaseRuntimeResources) can skip VM-dependent calls, e.g. volume unmounts, on a VM
    // that has exited. A graceful stop reaches here with the VM still alive (is_signaled() false), so
    // its mounts are unmounted through the live VM; StartVmLockHeld clears this for the next instance.
    if (m_vmExitedEvent && m_vmExitedEvent.is_signaled())
    {
        m_vmExited.store(true);
    }

    if (m_hooks.TearDownSessionState)
    {
        m_hooks.TearDownSessionState(CaptureTerminationReason);
    }

    m_volumes.reset();

    // Stop the IO relay.
    // This stops:
    // - container state monitoring.
    // - container init process relays
    // - execs relays
    // - container logs relays
    if (m_ioRelay)
    {
        m_ioRelay->Stop();
    }

    {
        std::lock_guard allocatedPortsLock(m_allocatedPortsLock);
        m_allocatedPorts.clear();
    }

    // The session-scoped event tracker is intentionally not reset. Its stream handle dies with the IO
    // relay above, and InitializeDockerRuntime re-binds it on the next start.
    m_dockerClient.reset();

    if (CaptureTerminationReason)
    {
        // Default: an explicit/graceful teardown is a shutdown (the VM is still alive and we are
        // bringing it down). Overridden below if the VM exited on its own and recorded a cause.
        m_lastTerminationReason = WSLCVirtualMachineTerminationReasonShutdown;
        m_lastTerminationDetails.clear();
    }

    // Check if the VM has already exited (e.g., killed externally).
    // If so, skip operations that require a live VM to avoid unnecessary waits.
    // N.B. m_vmExitedEvent may be uninitialized if teardown runs before GetTerminationEvent() succeeds.
    if (m_vmExitedEvent && m_vmExitedEvent.is_signaled())
    {
        WSL_LOG("SkippingGracefulShutdown_VmDead", TraceLoggingValue(m_id, "SessionId"));

        // The VM exited on its own, so it recorded the cause.
        if (CaptureTerminationReason && m_virtualMachine)
        {
            wil::unique_cotaskmem_string details;
            LOG_IF_FAILED(m_virtualMachine->GetTerminationReason(&m_lastTerminationReason, &details));
            m_lastTerminationDetails = details ? details.get() : L"";
        }
    }
    else if (m_virtualMachine)
    {
        m_virtualMachine->OnSessionTerminated();

        // Stop dockerd first, then containerd (dockerd is a client of containerd).
        // N.B. dockerd waits a couple seconds if there are any outstanding HTTP request sockets opened.
        if (m_dockerdProcess.has_value())
        {
            auto dockerdExitCode =
                StopProcess(m_dockerdProcess.value(), wsl::windows::wslc::ProcessTerminateTimeoutMs, wsl::windows::wslc::ProcessKillTimeoutMs);
            WSL_LOG("DockerdExit", TraceLoggingValue(dockerdExitCode, "code"));
        }

        if (m_containerdProcess.has_value())
        {
            auto containerdExitCode =
                StopProcess(m_containerdProcess.value(), wsl::windows::wslc::ProcessTerminateTimeoutMs, wsl::windows::wslc::ProcessKillTimeoutMs);
            WSL_LOG("ContainerdExit", TraceLoggingValue(containerdExitCode, "code"));
        }

        // N.B. dockerd has exited by this point, so unmounting the VHD is safe since no container can be running.
        if (m_storageMounted)
        {
            try
            {
                m_virtualMachine->Unmount(wsl::windows::wslc::ContainerdStorageMountPoint);
            }
            CATCH_LOG();
        }
    }

    m_dockerdProcess.reset();
    m_containerdProcess.reset();

    // Retire the lock-free mirror before destroying the object so an unlocked reader never sees
    // "a VM is present" while it is being torn down.
    m_hasVm.store(false);
    m_virtualMachine.reset();
    m_storageMounted = false;

    // Destroy the relay unless we're on its own thread (~IORelay joins the thread, which would
    // deadlock). On unexpected-VM-exit path (runs on relay thread), leave it for ~WSLCSession.
    if (!m_ioRelay || !m_ioRelay->IsRelayThread())
    {
        m_ioRelay.reset();
        m_vmExitedEvent.reset();
    }

    // Delete the ephemeral swap VHD now that the VM is gone.
    if (!m_swapVhdPath.empty())
    {
        LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_swapVhdPath.c_str()));
        m_swapVhdPath.clear();
    }
}

void WSLCSessionRuntime::OnIdleTimer()
try
{
    // Idle teardown releases cross-process COM proxies (the VM and its VM-scoped state), so this
    // threadpool callback must join the process MTA; otherwise those Release/calls fail with
    // RPC_E_WRONG_THREAD. The function-try-block keeps this (and everything below) under CATCH_LOG:
    // the threadpool callback that invokes us is noexcept, so an escaping throw would terminate.
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    if (m_terminating->load() || !IdleTerminationEnabled())
    {
        return;
    }

    // Non-blocking acquire: a blocking exclusive would queue behind in-flight operations, and
    // SRW locks favor waiting writers, stalling all new ops. If the lock is held, an operation
    // is in flight; it holds an activity reference and will re-arm the timer (via the 1->0
    // transition) when it releases, so there is nothing to do here.
    auto lock = m_lock.try_lock_exclusive();
    if (!lock)
    {
        return;
    }

    // Re-check every teardown precondition under the lock. The activity count is the single
    // source of truth for "the VM is needed"; a 0->1 transition since the timer fired (cancel
    // raced the callback) is caught here.
    if (m_terminating->load() || m_vmState.load() != VmState::Running || m_idleState->ActivityCount() != 0)
    {
        return;
    }

    // Claim the stop. If we lose, OnVmExited() owns a spontaneous-exit teardown and is spinning for
    // this lock, so release it and let that run instead of joining the relay ourselves.
    if (!TryClaimExpectedStop())
    {
        return;
    }

    // Restore Active on completion (or early exit) so the next StartVmLockHeld starts clean; only
    // clear our own claim.
    auto dispositionCleanup = wil::scope_exit([this]() {
        auto stopRequested = VmExitDisposition::StopRequested;
        m_vmExitDisposition.compare_exchange_strong(stopRequested, VmExitDisposition::Active);
    });

    // Final activity re-check under the lock, and the last point at which the stop can still be called
    // off: a VmLease that bumped the count since the check above is now blocked on the shared lock we
    // hold. Back out here -- nothing has been announced yet -- rather than stopping a VM that is about
    // to be used again.
    if (m_idleState->ActivityCount() != 0)
    {
        return;
    }

    // Past this point the VM is going away, whatever happens. Publishing the pending stop under the
    // lock is what makes the announcement below true: an ordinary lease that races in while the lock
    // is dropped no longer gets this VM, it waits for the teardown and is served by the next one. The
    // exception is a lease taken by the OnVmStopping handler itself, which must be served or it would
    // deadlock against the very callback this teardown is waiting on. Work that handler leaves running
    // dies with the VM -- which is exactly what it was just told would happen.
    //
    // N.B. endStop is declared after 'lock' so it runs *before* the lock is released (reverse
    // declaration order): waiters are released only once the teardown is complete and published.
    BeginVmStopLockHeld();
    auto endStop = wil::scope_exit([this]() { EndVmStop(); });

    // Fire OnVmStopping with m_lock dropped so a plugin handler may take a VM lease without
    // deadlocking, and while the VM is still running so the handler can still use it.
    //
    // The notification cannot be allowed to abort the teardown: the stop is already published and its
    // generation retired, so bailing out here would leave waiters to be served by the VM they were
    // promised would die, and its eventual teardown would be silent.
    const auto generation = m_vmGeneration.load();

    lock.reset();
    try
    {
        NotifyVmStopping(generation);
    }
    CATCH_LOG();
    lock = m_lock.lock_exclusive();

    // Unconditional: the stop was announced, so it happens. Reacquiring the exclusive lock first
    // drains every in-flight operation, so nothing is cut off mid-call. StopVmLockHeld no-ops if a
    // concurrent Terminate already tore the VM down, and copes with a VM that crashed while the lock
    // was dropped -- OnVmExited() declined that exit (we hold the expected-stop claim) and its exit
    // handle is one-shot, so this is the only teardown left to run.
    StopVmLockHeld();
}
CATCH_LOG();

bool WSLCSessionRuntime::TriggerIdleTerminationForTest()
{
    // Mirror OnIdleTimer's MTA context on a dedicated thread: the incoming RPC thread is an STA, so
    // both re-initializing MTA on it and running teardown inline would fail (RPC_E_CHANGED_MODE /
    // RPC_E_WRONG_THREAD when releasing the VM's cross-process COM proxies).
    bool wasAlreadyIdle = false;
    std::exception_ptr error;

    std::thread worker([&]() {
        try
        {
            const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

            auto lock = m_lock.lock_exclusive();

            if (m_terminating->load() || m_vmState.load() != VmState::Running)
            {
                wasAlreadyIdle = true;
                return;
            }

            // tmpfs sessions have no persistent storage to recover from, so tearing a running VM down
            // loses all state. Check this after the VM state so a never-started session still reports
            // that it was already idle.
            if (!IdleTerminationEnabled())
            {
                return;
            }

            // Match the production idle timer: an active container or operation keeps the VM alive.
            if (m_idleState->ActivityCount() != 0)
            {
                return;
            }

            if (!TryClaimExpectedStop())
            {
                wasAlreadyIdle = true;
                return;
            }

            auto dispositionCleanup = wil::scope_exit([this]() {
                auto stopRequested = VmExitDisposition::StopRequested;
                m_vmExitDisposition.compare_exchange_strong(stopRequested, VmExitDisposition::Active);
            });

            // Mirror OnIdleTimer's final re-check under the lock: a lease taken while the stop claim
            // was being made is now blocked on the lock we hold, and must back the teardown out here
            // rather than announce a stop production code would have suppressed.
            if (m_idleState->ActivityCount() != 0)
            {
                return;
            }

            // Commit to the stop before announcing it, then fire OnVmStopping without holding m_lock
            // and tear down unconditionally. See OnIdleTimer for the full rationale.
            BeginVmStopLockHeld();
            auto endStop = wil::scope_exit([this]() { EndVmStop(); });

            const auto generation = m_vmGeneration.load();

            lock.reset();
            try
            {
                NotifyVmStopping(generation);
            }
            CATCH_LOG();
            lock = m_lock.lock_exclusive();

            StopVmLockHeld();
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });

    worker.join();

    if (error)
    {
        std::rethrow_exception(error);
    }

    return wasAlreadyIdle;
}

WSLCSessionRuntime::VmLease WSLCSessionRuntime::AcquireVmLease(VmLeasePolicy Policy)
{
    return VmLease(*this, Policy);
}

WSLCSessionRuntime::LockedRuntime WSLCSessionRuntime::Acquire(VmLeasePolicy Policy)
{
    return LockedRuntime(*this, Policy);
}

WSLCSessionRuntime::VmLease::VmLease(WSLCSessionRuntime& Runtime, VmLeasePolicy Policy) : m_runtime(&Runtime)
{
    // Record an in-flight operation before bringing the VM up so idle teardown cannot tear it down
    // between EnsureVmRunning() and acquiring the shared lock. AddActivity cancels any pending idle
    // timer.
    m_runtime->m_idleState->AddActivity();

    auto countCleanup = wil::scope_exit([this]() {
        m_runtime->m_idleState->ReleaseActivity();
        m_runtime = nullptr;
    });

    // Activity increment may race with idle teardown. Retry until we hold the lock with VM running.
    for (;;)
    {
        if (Policy == VmLeasePolicy::Acquire)
        {
            m_runtime->EnsureVmRunning();
        }

        m_lock = m_runtime->m_lock.lock_shared();

        if (m_runtime->m_vmState.load() == VmState::Running)
        {
            // An announced stop always happens, so a VM with one pending is unusable even though it is
            // still Running: wait for the teardown, then retry, which brings up a fresh VM. ExistingOnly
            // callers are exempt and are served by the stopping VM -- see VmLeasePolicy.
            if (Policy == VmLeasePolicy::ExistingOnly || !m_runtime->m_vmStopPending.load())
            {
                break;
            }

            // Release the shared lock before waiting. The teardown must be able to take the exclusive
            // lock, and it is the only thing that will ever signal us.
            m_lock.reset();

            // The wait is bounded only so that a handler wedging the teardown is visible in traces;
            // there is no correct way to proceed without a VM, so the retry is unconditional.
            while (!m_runtime->m_vmStopCompleteEvent.wait(c_vmStopWaitLogIntervalMs))
            {
                WSL_LOG(
                    "WslcVmLeaseWaitingForStop",
                    TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
                    TraceLoggingValue(m_runtime->m_id, "SessionId"));
            }

            continue;
        }

        // ExistingOnly never starts a VM, so retrying could only spin. Reject the caller instead.
        THROW_HR_IF(WSLC_E_VM_NOT_RUNNING, Policy == VmLeasePolicy::ExistingOnly);

        WSL_LOG(
            "WslcVmLeaseRetry",
            TraceLoggingValue(m_runtime->m_id, "SessionId"),
            TraceLoggingValue(static_cast<uint32_t>(m_runtime->m_vmState.load()), "VmState"));
        m_lock.reset();
    }

    countCleanup.release();
}

WSLCSessionRuntime::VmLease::VmLease(VmLease&& Other) noexcept :
    m_runtime(std::exchange(Other.m_runtime, nullptr)), m_lock(std::move(Other.m_lock))
{
}

WSLCSessionRuntime::VmLease& WSLCSessionRuntime::VmLease::operator=(VmLease&& Other) noexcept
{
    if (this != &Other)
    {
        if (m_runtime != nullptr)
        {
            // Release the shared lock before the activity reference so that, if this was the last
            // activity, idle teardown can immediately take the exclusive lock.
            m_lock.reset();
            m_runtime->m_idleState->ReleaseActivity();
        }

        m_runtime = std::exchange(Other.m_runtime, nullptr);
        m_lock = std::move(Other.m_lock);
    }

    return *this;
}

WSLCSessionRuntime::VmLease::~VmLease()
{
    if (m_runtime != nullptr)
    {
        // Release the shared lock before the activity reference so that, if this was the last
        // activity, idle teardown can immediately take the exclusive lock. ReleaseActivity arms the
        // idle timer on the 1->0 transition.
        m_lock.reset();
        m_runtime->m_idleState->ReleaseActivity();
    }
}

WSLCSessionRuntime::LockedRuntime::LockedRuntime(WSLCSessionRuntime& Runtime, VmLeasePolicy Policy) :
    m_runtime(&Runtime), m_lease(Runtime.AcquireVmLease(Policy))
{
}

WSLCVirtualMachine& WSLCSessionRuntime::LockedRuntime::Vm()
{
    return m_runtime->Vm();
}

IORelay* WSLCSessionRuntime::LockedRuntime::Relay()
{
    return m_runtime->Relay();
}

DockerHTTPClient& WSLCSessionRuntime::LockedRuntime::Docker()
{
    return m_runtime->Docker();
}

void WSLCSessionRuntime::OnVmExited()
{
    // A spontaneous exit we must permanently terminate, unless an expected stop already claimed it,
    // in which case the exit was wanted and we decline.
    if (!TryClaimSpontaneousExit())
    {
        WSL_LOG("WslcVmExitedDuringStop", TraceLoggingValue(m_id, "SessionId"));
        return;
    }

    WSL_LOG(
        "VmExited",
        TraceLoggingLevel(WINEVENT_LEVEL_WARNING),
        TraceLoggingValue(m_id, "SessionId"),
        TraceLoggingValue(m_displayName.c_str(), "Name"),
        TraceLoggingValue(!m_sessionTerminatingEvent.is_signaled(), "Unexpected"));

    if (m_hooks.OnSpontaneousExit)
    {
        m_hooks.OnSpontaneousExit();
    }
}

void WSLCSessionRuntime::Shutdown(
    wil::rwlock_release_exclusive_scope_exit& runtimeLock, WSLCVirtualMachineTerminationReason& terminationReason, std::wstring& terminationDetails)
{
    if (!m_initialized)
    {
        return;
    }

    // runtimeLock is an exclusive hold on m_lock, guaranteeing no operation is running.
    WI_VERIFY(runtimeLock);

    // Permanently disable idle teardown and drain any in-flight timer callback on every exit path,
    // including an exception escaping the teardown below. The timer callback captures this runtime,
    // but IdleState outlives it (activity tokens hold shared_ptr copies), so skipping the disarm
    // would leave a late 1->0 transition able to re-arm a timer that references freed memory.
    //
    // The exclusive lock is released first: a timer callback already blocked acquiring it must be
    // able to obtain it, observe m_terminating, and return, otherwise the drain below deadlocks.
    auto disarmIdleState = wil::scope_exit([&]() {
        runtimeLock.reset();
        m_idleState->Disarm();
    });

    // Notify with m_lock dropped, then re-lock for the teardown. The handler may call back into the
    // session (e.g. WSLCCreateProcess) which takes a VM lease and this lock; firing under it would
    // deadlock. m_terminating is set, so any such reentrant lease fails at EnsureVmRunning's gate
    // rather than restarting the VM, and the reacquire can't block on it. A throwing handler must not
    // skip the teardown below -- the session is terminating either way.
    runtimeLock.reset();
    try
    {
        NotifyVmStopping(m_vmGeneration.load());
    }
    CATCH_LOG();
    runtimeLock = m_lock.lock_exclusive();

    // Tear down the VM (if running) and all VM-scoped state, capturing the termination reason; the
    // reacquired exclusive hold on m_lock satisfies TearDownVmLockHeld's precondition.
    TearDownVmLockHeld(/* CaptureTerminationReason */ true);

    m_vmState.store(VmState::None);

    terminationReason = m_lastTerminationReason;
    terminationDetails = m_lastTerminationDetails;

    // Signal completion last so any observer of the terminated event sees a fully torn-down
    // session and a populated termination reason.
    m_sessionTerminatedEvent.SetEvent();
}

} // namespace wsl::windows::service::wslc
