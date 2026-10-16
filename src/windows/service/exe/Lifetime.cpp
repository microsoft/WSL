/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Lifetime.cpp

Abstract:

    This file contains function definitions around client lifetime.

--*/

#include "precomp.h"
#include "Lifetime.h"

#define RETRY_TIMER_PERIOD (60 * 1000)
#define RETRY_TIMER_WINDOW (1000)

static bool IsSameProcess(_In_ HANDLE process1, _In_ HANDLE process2)
{
    const DWORD pid1 = GetProcessId(process1);
    THROW_LAST_ERROR_IF(pid1 == 0);

    const DWORD pid2 = GetProcessId(process2);
    THROW_LAST_ERROR_IF(pid2 == 0);

    return pid1 == pid2;
}

LifetimeManager::LifetimeManager() : m_nextClientKey(0)
{
    m_cleanupWork.reset(CreateThreadpoolWork(s_DrainRetiredCallbacks, this, nullptr));
    THROW_LAST_ERROR_IF(!m_cleanupWork);
}

LifetimeManager::~LifetimeManager()
{
    if (wil::ProcessShutdownInProgress())
    {
        return;
    }

    ClearCallbacks();
}

void LifetimeManager::ClearCallbacks()
{
    wil::unique_threadpool_wait lastCallbackWait;
    wil::unique_threadpool_timer lastTimerWait;
    {
        std::lock_guard<std::mutex> lock(m_lock);

        // Set m_exiting to make sure no new callbacks can be scheduled.
        m_exiting = true;

        while (!m_callbackList.empty())
        {
            _RetireCallback(m_callbackList.begin());
        }

        lastCallbackWait = std::move(m_lastCallbackWait);
        lastTimerWait = std::move(m_lastTimerWait);
    }

    // Also drain registrations removed before ClearCallbacks(), including callbacks
    // that have not acquired m_lock yet. Do not cancel the cleanup work itself.
    WaitForThreadpoolWorkCallbacks(m_cleanupWork.get(), FALSE);
}

ULONG64 LifetimeManager::GetRegistrationId()
{
    std::lock_guard<std::mutex> lock(m_lock);
    THROW_IF_FAILED(ULong64Add(m_nextClientKey, 1, &m_nextClientKey));

    return m_nextClientKey;
}

bool LifetimeManager::IsAnyProcessRegistered(_In_ ULONG64 ClientKey)
{
    std::lock_guard<std::mutex> lock(m_lock);
    const auto client = _FindClient(ClientKey);
    return (client != m_callbackList.end());
}

void LifetimeManager::RegisterCallback(_In_ ULONG64 ClientKey, _In_ const std::function<bool(void)>& Callback, _In_opt_ HANDLE ClientProcess, _In_ DWORD TimeoutMs)
{
    std::lock_guard<std::mutex> lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_SHUTDOWN_IN_PROGRESS), m_exiting);
    auto client = _FindClient(ClientKey);
    if (client == m_callbackList.end())
    {
        ClientCallback newClient{};
        newClient.callback = std::move(Callback);
        newClient.clientKey = ClientKey;
        newClient.timeout = TimeoutMs;
        newClient.CreateTimer(s_OnTimeout, this);
        m_callbackList.emplace_back(std::move(newClient));
        client = _FindClient(ClientKey);
        if (!ARGUMENT_PRESENT(ClientProcess))
        {
            client->SetTimer(TimeoutMs);
        }
    }
    else
    {
        // If a client was found, update the callback and timeout and cancel
        // any pending timer.
        client->callback = std::move(Callback);
        client->timeout = TimeoutMs;
        if (ARGUMENT_PRESENT(ClientProcess))
        {
            client->CancelTimer();
        }
    }

    WI_ASSERT(client != m_callbackList.end());

    if (ARGUMENT_PRESENT(ClientProcess))
    {
        const auto proc = client->FindProcess(ClientProcess);
        if (proc == client->clientProcesses.end())
        {
            OwnedProcess newProcess{};
            newProcess.process.reset(wsl::windows::common::wslutil::DuplicateHandle(ClientProcess));
            newProcess.InitializeListenForTermination(s_OnClientProcessTerminated, this);
            client->clientProcesses.emplace_back(std::move(newProcess));
            client->clientProcesses.back().ListenForTermination();
        }
    }
}

bool LifetimeManager::RemoveCallback(_In_ ULONG64 ClientKey)
{
    bool callbackFound = false;
    std::lock_guard<std::mutex> lock(m_lock);
    const auto client = _FindClient(ClientKey);
    if (client != m_callbackList.end())
    {
        _RetireCallback(client);
        callbackFound = true;
    }

    return callbackFound;
}

_Requires_lock_held_(m_lock)
void LifetimeManager::_RetireCallback(std::list<ClientCallback>::iterator Client)
{
    Client->CancelTimer();
    for (const auto& process : Client->clientProcesses)
    {
        SetThreadpoolWait(process.terminationWait.get(), nullptr, nullptr);
    }

    m_retiredCallbacks.splice(m_retiredCallbacks.end(), m_callbackList, Client);
    SubmitThreadpoolWork(m_cleanupWork.get());
}

VOID CALLBACK LifetimeManager::s_DrainRetiredCallbacks(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_WORK)
{
    try
    {
        const auto manager = static_cast<LifetimeManager*>(Context);
        std::list<ClientCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(manager->m_lock);
            callbacks.splice(callbacks.end(), manager->m_retiredCallbacks);
        }

        for (auto& callback : callbacks)
        {
            wil::unique_threadpool_timer timer{callback.timer.release()};
            for (auto& process : callback.clientProcesses)
            {
                wil::unique_threadpool_wait wait{process.terminationWait.release()};
            }
        }
    }
    CATCH_FAIL_FAST()
}

VOID CALLBACK LifetimeManager::s_OnClientProcessTerminated(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult)
{
    UNREFERENCED_PARAMETER(WaitResult);
    WI_ASSERT(WaitResult == WAIT_OBJECT_0);

    try
    {
        const auto manager = static_cast<LifetimeManager*>(Context);
        std::function<bool(void)> callback;
        wil::unique_threadpool_wait previousCallbackWait{};

        // Search for a callback with a matching threadpool wait.
        {
            std::list<OwnedProcess>::iterator proc;
            std::lock_guard<std::mutex> lock(manager->m_lock);
            const auto client = std::find_if(manager->m_callbackList.begin(), manager->m_callbackList.end(), [&](ClientCallback& c) {
                proc = std::find_if(c.clientProcesses.begin(), c.clientProcesses.end(), [&Wait](const OwnedProcess& p) {
                    return (p.terminationWait.get() == Wait);
                });

                return (proc != c.clientProcesses.end());
            });

            if (client != manager->m_callbackList.end())
            {
                previousCallbackWait.reset(proc->terminationWait.release());
                previousCallbackWait.swap(manager->m_lastCallbackWait);

                // If this is the last client process, execute the callback
                // or queue a timer if a timeout was specified.
                //
                // N.B. The callback must be executed after dropping the lock.
                client->clientProcesses.erase(proc);
                if (client->clientProcesses.empty())
                {
                    if (client->timeout == 0)
                    {
                        callback = std::move(client->callback);
                        manager->_RetireCallback(client);
                    }
                    else
                    {
                        client->SetTimer(client->timeout);
                    }
                }
            }
        }

        // Callbacks that have a zero timeout must return success because they
        // are not retried.
        if (callback)
        {
            WI_VERIFY(callback());
        }
    }
    CATCH_LOG()
}

VOID CALLBACK LifetimeManager::s_OnTimeout(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER Timer)
{
    try
    {
        const auto manager = static_cast<LifetimeManager*>(Context);
        ClientCallback clientLocal;
        wil::unique_threadpool_timer previousTimerWait{};

        // Search for a callback with a matching timer.
        {
            std::lock_guard<std::mutex> lock(manager->m_lock);
            const auto client =
                std::find_if(manager->m_callbackList.begin(), manager->m_callbackList.end(), [Timer](const ClientCallback& c) {
                    return (Timer == c.timer.get());
                });

            if ((client != manager->m_callbackList.end()) && (client->clientProcesses.empty()))
            {
                clientLocal = std::move(*client);
                manager->m_callbackList.erase(client);
            }

            // If we took ownership of the timer (moved into clientLocal), stash it so the
            // destructor waits for this callback to finish before exiting. Otherwise this
            // firing is a no-op (e.g., the entry was erased, the timer was replaced, or the
            // matched entry still has live processes).
            if (clientLocal.timer)
            {
                clientLocal.CancelTimer();
                previousTimerWait.reset(clientLocal.timer.release());
                previousTimerWait.swap(manager->m_lastTimerWait);
            }
        }

        // If a callback was found, execute it. If the callback succeeds the
        // timer is cancelled. Otherwise, the callback is retried.
        //
        // N.B. The callback must be executed after dropping the lock.
        if (clientLocal.callback)
        {
            if (!clientLocal.callback())
            {
                std::lock_guard<std::mutex> lock(manager->m_lock);

                // Only re-queue the timer if not exiting (see ClearCallbacks())
                if (!manager->m_exiting)
                {
                    clientLocal.CreateTimer(s_OnTimeout, manager);
                    manager->m_callbackList.emplace_back(std::move(clientLocal));
                    const auto& client = manager->m_callbackList.back();
                    client.SetTimer(client.timeout);
                }
            }
        }
    }
    CATCH_LOG()
}

_Requires_lock_held_(m_lock)
std::list<LifetimeManager::ClientCallback>::iterator LifetimeManager::_FindClient(_In_ ULONG64 ClientKey)
{
    return std::find_if(m_callbackList.begin(), m_callbackList.end(), [&ClientKey](const ClientCallback& c) {
        return (ClientKey == c.clientKey);
    });
}

LifetimeManager::OwnedProcess::OwnedProcess()
{
}

LifetimeManager::OwnedProcess::~OwnedProcess()
{
}

LifetimeManager::OwnedProcess::OwnedProcess(OwnedProcess&& other) noexcept
{
    *this = std::move(other);
}

void LifetimeManager::OwnedProcess::operator=(OwnedProcess&& source) noexcept
{
    process = std::move(source.process);
    terminationWait = std::move(source.terminationWait);
}

void LifetimeManager::OwnedProcess::InitializeListenForTermination(_In_ PTP_WAIT_CALLBACK Callback, _In_ PVOID Context)
{
    terminationWait.reset(CreateThreadpoolWait(Callback, Context, nullptr));
    THROW_LAST_ERROR_IF(!terminationWait);
}

void LifetimeManager::OwnedProcess::ListenForTermination() const
{
    SetThreadpoolWait(terminationWait.get(), process.get(), nullptr);
}

LifetimeManager::ClientCallback::ClientCallback()
{
}

LifetimeManager::ClientCallback::~ClientCallback()
{
}

LifetimeManager::ClientCallback::ClientCallback(ClientCallback&& other) noexcept
{
    *this = std::move(other);
}

void LifetimeManager::ClientCallback::operator=(ClientCallback&& source) noexcept
{
    timer = std::move(source.timer);
    clientKey = source.clientKey;
    callback = std::move(source.callback);
    timeout = source.timeout;
    clientProcesses = std::move(source.clientProcesses);
}

void LifetimeManager::ClientCallback::CancelTimer() const
{
    SetThreadpoolTimer(timer.get(), nullptr, 0, 0);
}

void LifetimeManager::ClientCallback::CreateTimer(_In_ PTP_TIMER_CALLBACK Callback, _In_ PVOID Context)
{
    timer.reset(CreateThreadpoolTimer(Callback, Context, nullptr));
    THROW_LAST_ERROR_IF(!timer);
}

std::list<LifetimeManager::OwnedProcess>::iterator LifetimeManager::ClientCallback::FindProcess(_In_ HANDLE Process)
{
    return std::find_if(clientProcesses.begin(), clientProcesses.end(), [&Process](const OwnedProcess& p) {
        return IsSameProcess(Process, p.process.get());
    });
}

void LifetimeManager::ClientCallback::SetTimer(_In_ DWORD DueTimeMs) const
{
    FILETIME dueTime = wil::filetime::from_int64(static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_millisecond * DueTimeMs));
    SetThreadpoolTimer(timer.get(), &dueTime, RETRY_TIMER_PERIOD, RETRY_TIMER_WINDOW);
}
