/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Lifetime.h

Abstract:

    This file contains function declarations around client lifetime.

--*/

#pragma once
#include <mutex>
#include <wil/resource.h>

class LifetimeManager
{
public:
    LifetimeManager();
    ~LifetimeManager();

    LifetimeManager(const LifetimeManager&) = delete;
    void operator=(const LifetimeManager&) = delete;
    LifetimeManager(LifetimeManager&& source) = delete;

    ULONG64 GetRegistrationId();

    bool IsAnyProcessRegistered(_In_ ULONG64 ClientKey);

    void RegisterCallback(_In_ ULONG64 ClientKey, _In_ const std::function<bool(void)>& Callback, _In_opt_ HANDLE ClientProcess, _In_ DWORD TimeoutMs = 0);

    bool RemoveCallback(_In_ ULONG64 ClientKey);

    void ClearCallbacks();

    struct OwnedProcess
    {
        OwnedProcess();
        ~OwnedProcess();
        OwnedProcess(OwnedProcess&& other) noexcept;
        void operator=(OwnedProcess&&) noexcept;
        OwnedProcess(const OwnedProcess&) = delete;
        void operator=(const OwnedProcess&) = delete;

        void InitializeListenForTermination(_In_ PTP_WAIT_CALLBACK Callback, _In_ PVOID Context);
        void ListenForTermination() const;

        wil::unique_handle process;
        wil::unique_threadpool_wait_nowait terminationWait;
    };

    struct ClientCallback
    {
        ClientCallback();
        ~ClientCallback();
        ClientCallback(ClientCallback&& other) noexcept;
        void operator=(ClientCallback&&) noexcept;
        ClientCallback(const ClientCallback&) = delete;
        void operator=(const ClientCallback&) = delete;

        void CancelTimer() const;
        void CreateTimer(_In_ PTP_TIMER_CALLBACK Callback, _In_ PVOID Context);
        std::list<OwnedProcess>::iterator FindProcess(_In_ HANDLE Process);
        void SetTimer(_In_ DWORD DueTimeMs) const;

        std::list<OwnedProcess> clientProcesses;
        wil::unique_threadpool_timer_nowait timer;
        ULONG64 clientKey{};
        std::function<bool(void)> callback;
        DWORD timeout{};
    };

private:
    _Requires_lock_held_(m_lock)
    std::list<ClientCallback>::iterator _FindClient(_In_ ULONG64 ClientKey);

    static VOID CALLBACK s_OnClientProcessTerminated(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult);

    static VOID CALLBACK s_OnTimeout(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER Timer);

    std::mutex m_lock;

    _Guarded_by_(m_lock) bool m_exiting = false;

    _Guarded_by_(m_lock) ULONG64 m_nextClientKey;

    _Guarded_by_(m_lock) std::list<ClientCallback> m_callbackList;

    // N.B. There is a race that could cause AV between callbacks firing and
    //      the destruction of the lifetime manager class. To avoid the race
    //      create a chain of waits where each callback waits for the previous
    //      callback to finish. The destructor of the class waits on the final
    //      callback before returning.
    wil::unique_threadpool_wait m_lastCallbackWait;
    wil::unique_threadpool_timer m_lastTimerWait;
};
