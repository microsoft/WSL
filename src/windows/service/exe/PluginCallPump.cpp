// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "PluginCallPump.h"
#include <thread>

using wsl::windows::service::PluginCallPump;

namespace {

HRESULT InvokeWork(const std::function<HRESULT()>& Work)
try
{
    return Work();
}
CATCH_RETURN()

} // namespace

void PluginCallPump::Drain()
{
    std::deque<Call*> calls;
    {
        auto lock = m_lock.lock_exclusive();
        calls.swap(m_queue);
    }
    for (auto* call : calls)
    {
        call->result = InvokeWork(call->work);
        call->done.SetEvent();
    }
}

void PluginCallPump::Stop() noexcept
{
    auto lock = m_lock.lock_exclusive();
    m_stopped = true;
    for (auto* call : m_queue)
    {
        call->result = RPC_E_DISCONNECTED;
        call->done.SetEvent();
    }
    m_queue.clear();
}

HRESULT PluginCallPump::Run(const std::function<HRESULT()>& Notification)
try
{
    std::thread worker;
    wil::unique_event finished;
    HRESULT result = E_UNEXPECTED;
    auto cleanup = wil::scope_exit([&] {
        // Stop before joining: the worker may itself be waiting for a callback.
        Stop();
        if (worker.joinable())
        {
            worker.join();
        }
    });

    finished.create(wil::EventOptions::ManualReset);
    worker = std::thread([&] {
        result = InvokeWork(Notification);
        finished.SetEvent();
    });

    const HANDLE handles[] = {finished.get(), m_available.get()};
    for (;;)
    {
        const auto wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);
        THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
        if (wait == WAIT_OBJECT_0)
        {
            {
                auto lock = m_lock.lock_exclusive();
                m_stopped = true;
            }
            Drain();
            worker.join();
            return result;
        }
        Drain();
    }
}
CATCH_RETURN()

bool PluginCallPump::Invoke(const std::function<HRESULT()>& Work, HRESULT& Result)
{
    Call call{Work};
    {
        auto lock = m_lock.lock_exclusive();
        if (m_stopped)
        {
            return false;
        }
        m_queue.push_back(&call);
        m_available.SetEvent();
    }
    call.done.wait();
    Result = call.result;
    return true;
}
