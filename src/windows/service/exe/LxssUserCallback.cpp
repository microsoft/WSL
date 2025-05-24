/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserCallback.cpp

Abstract:

    This file contains kernel->user callback function definitions.

--*/

#include "precomp.h"
#include "LxssUserCallback.h"

LxssUserCallback::LxssUserCallback(_In_ HANDLE Handle, _In_ LXBUS_USER_CALLBACK_TYPE CallbackType, _In_ const LXSS_USER_CALLBACK& Callback, _In_ ULONG OutputBufferSize) :
    m_callback(Callback), m_exiting(false), m_callbackType(CallbackType), m_event(wil::EventOptions::ManualReset | wil::EventOptions::Signaled)
{
    // Keep a local copy of the handle so the request can be requeued.
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateHandle(GetCurrentProcess(), Handle, GetCurrentProcess(), &m_handle, 0, TRUE, DUPLICATE_SAME_ACCESS));

    // All result buffers are derivatives of LXBUS_USER_CALLBACK_DATA.
    WI_ASSERT(OutputBufferSize >= sizeof(LXBUS_USER_CALLBACK_DATA));

    // Allocate a buffer of the requested size.
    m_buffer.resize(OutputBufferSize);

    // Set up the threadpool wait callback.
    PTP_WAIT_CALLBACK ThreadpoolCallback;
    ThreadpoolCallback = reinterpret_cast<PTP_WAIT_CALLBACK>(&ThreadpoolCallbackProxy);

    // N.B. Using unreferenced 'this' as context parameter since the destructor
    //      should wait for the threadpool thread to complete/unregister.
    m_threadpoolWait.reset(CreateThreadpoolWait(ThreadpoolCallback, this, nullptr));

    THROW_LAST_ERROR_IF(!m_threadpoolWait);
    return;
}

LxssUserCallback::~LxssUserCallback()
{
    {
        // synchronize with the callback thread to avoid a race where m_event
        // is signalled but the callback is in the middle of queueing up
        // another IO request.
        std::lock_guard<std::mutex> lock(m_lock);
        m_exiting = true;
    }

    SetThreadpoolWait(m_threadpoolWait.get(), nullptr, nullptr);
    IO_STATUS_BLOCK ioCancelStatus{};
    const NTSTATUS status = NtCancelIoFileEx(m_handle.get(), &m_ioStatus, &ioCancelStatus);

    // If the instance has been terminated, the request may already have been
    // cancelled.
    if (status != STATUS_NOT_FOUND)
    {
        LOG_IF_NTSTATUS_FAILED_MSG(status, "Failed to cancel user callback IO");
    }

    // Wait for outstanding IO to complete since it references memory owned by
    // this instance.
    m_event.wait();
}

VOID LxssUserCallback::QueueRequest()
{
    auto setEventOnFailure = m_event.SetEvent_scope_exit();
    m_event.ResetEvent();
    LXBUS_REGISTER_USER_CALLBACK_PARAMETERS parameters;
    parameters.Input.CallbackType = m_callbackType;
    const ULONG outputBufferSize = std::min<ULONG>(static_cast<ULONG>(m_buffer.size()), ULONG_MAX);

    THROW_IF_NTSTATUS_FAILED(LxBusClientRegisterUserCallbackAsync(
        m_handle.get(), m_event.get(), &m_ioStatus, &parameters, &m_buffer.front(), outputBufferSize));

    SetThreadpoolWait(m_threadpoolWait.get(), m_event.get(), nullptr);
    setEventOnFailure.release();
}

std::unique_ptr<LxssUserCallback> LxssUserCallback::Register(
    _In_ HANDLE Handle, _In_ LXBUS_USER_CALLBACK_TYPE CallbackType, _In_ const LXSS_USER_CALLBACK& Callback, _In_ ULONG OutputBufferSize)
{
    std::unique_ptr<LxssUserCallback> userCallback(new LxssUserCallback(Handle, CallbackType, Callback, OutputBufferSize));

    userCallback->QueueRequest();
    return userCallback;
}

VOID LxssUserCallback::ThreadpoolCallback(_Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(WaitResult);

    WI_ASSERT(Wait == m_threadpoolWait.get());
    WI_ASSERT(WaitResult == WAIT_OBJECT_0);

    if (NT_SUCCESS(m_ioStatus.Status))
    {
        WI_ASSERT(m_ioStatus.Information >= sizeof(LXBUS_USER_CALLBACK_DATA));

        const auto callbackData = static_cast<PLXBUS_USER_CALLBACK_DATA>(static_cast<PVOID>(&m_buffer.front()));

        const unsigned long long callbackId = callbackData->CallbackId;
        NTSTATUS status = STATUS_INTERNAL_ERROR;
        try
        {
            status = m_callback(&m_buffer.front(), m_ioStatus.Information);
        }
        CATCH_LOG()

        LXBUS_REGISTER_USER_CALLBACK_PARAMETERS parameters{};
        parameters.Input.CallbackType = LxBusUserCallbackTypeResult;
        parameters.Input.ResultData.CallbackId = callbackId;
        parameters.Input.ResultData.Result = status;
        LOG_IF_NTSTATUS_FAILED(LxBusClientUserCallbackSendResponse(m_handle.get(), &parameters));
    }
    else if (m_ioStatus.Status == STATUS_CANCELLED)
    {
        // Don't queue another request if the previous one was canceled.
        // Cancel should only occur when the instance is shutting down or
        // when the destructor runs.
        return;
    }
    else
    {
        LOG_NTSTATUS_MSG(m_ioStatus.Status, "User callback IO completed with failure");
    }

    // Requeue another request.
    //
    // N.B. If queueing the request fails, the instance will no longer be able
    //      to perform up-calls to the usermode service for this type of
    //      operation. This is benign if the request fails because rundown
    //      could not be acquired due to the instance terminating.
    //
    // TODO_LX: use telemetry to determine what other failures can occur so
    //          can be handled gracefully.
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_exiting)
        {
            try
            {
                QueueRequest();
            }
            CATCH_LOG()
        }
    }
}

VOID CALLBACK LxssUserCallback::ThreadpoolCallbackProxy(
    _Inout_ PTP_CALLBACK_INSTANCE Instance, _Inout_opt_ PVOID Context, _Inout_ PTP_WAIT Wait, _In_ TP_WAIT_RESULT WaitResult)
{
    const auto Self = static_cast<LxssUserCallback*>(Context);
    Self->ThreadpoolCallback(Instance, Wait, WaitResult);
}
