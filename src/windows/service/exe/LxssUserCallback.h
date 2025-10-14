/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserCallback.h

Abstract:

    This file contains kernel->user callback function declarations.

--*/

#pragma once
#include <mutex>
#include <wil/resource.h>

using LXSS_USER_CALLBACK = std::function<NTSTATUS(PVOID CallbackBuffer, ULONG_PTR CallbackBufferSize)>;

class LxssUserCallback
{
public:
    /// <summary>
    /// Destructor.
    /// </summary>
    ~LxssUserCallback();

    /// <summary>
    /// Register a new user callback handler.
    /// </summary>
    static std::unique_ptr<LxssUserCallback> Register(
        _In_ HANDLE Handle, _In_ LXBUS_USER_CALLBACK_TYPE CallbackType, _In_ const LXSS_USER_CALLBACK& Callback, _In_ ULONG OutputBufferSize);

private:
    /// <summary>
    /// No default constructor.
    /// </summary>
    LxssUserCallback() = delete;
    /// <summary>
    /// No copy constructor.
    /// </summary>
    LxssUserCallback(const LxssUserCallback&) = delete;

    /// <summary>
    /// Private constructor.
    /// </summary>
    LxssUserCallback(_In_ HANDLE Handle, _In_ LXBUS_USER_CALLBACK_TYPE CallbackType, _In_ const LXSS_USER_CALLBACK& Callback, _In_ ULONG OutputBufferSize);

    /// <summary>
    /// Queue the request with the kernel driver.
    /// </summary>
    VOID QueueRequest();

    /// <summary>
    /// Threadpool callback handler.
    /// </summary>
    VOID ThreadpoolCallback(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult);

    /// <summary>
    /// Threadpool callback entry.
    /// </summary>
    static VOID CALLBACK ThreadpoolCallbackProxy(
        _Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult);

    /// <summary>
    /// Output buffer.
    /// </summary>
    std::vector<BYTE> m_buffer;

    /// <summary>
    /// Reference to callback function.
    /// </summary>
    LXSS_USER_CALLBACK m_callback;

    /// <summary>
    /// Set when instance is destructing. Should be accessed holding m_lock.
    /// </summary>
    bool m_exiting;

    /// <summary>
    /// The type of callback to register.
    /// </summary>
    LXBUS_USER_CALLBACK_TYPE m_callbackType;

    /// <summary>
    /// Event triggered when asynchronous callback IOCTL is completed.
    /// </summary>
    wil::unique_event m_event;

    /// <summary>
    /// Stores handle to use to send IOCTL to kernel driver.
    /// </summary>
    wil::unique_handle m_handle;

    /// <summary>
    /// IO status.
    /// </summary>
    IO_STATUS_BLOCK m_ioStatus;

    /// <summary>
    /// Synchronize access to certain fields.
    /// </summary>
    std::mutex m_lock;

    /// <summary>
    /// Threadpool IO handle.
    /// </summary>
    // N.B. Keep this as the last member so that it is the first to be
    //      destructed. This will ensure that if the threadpool callback
    //      races with instance destruction, all of the fields will be valid.
    //      At completion of this member's destruction, any outstanding
    //      threadpool thread should have completed and the callback
    //      will have been unregistered.
    wil::unique_threadpool_wait m_threadpoolWait;
};
