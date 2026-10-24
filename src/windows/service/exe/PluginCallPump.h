// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <deque>
#include <functional>
#include <wil/resource.h>

namespace wsl::windows::service {

// Runs callbacks on the notifying thread so they can re-enter its session lock.
class PluginCallPump
{
public:
    NON_COPYABLE(PluginCallPump);
    NON_MOVABLE(PluginCallPump);

    PluginCallPump() = default;
    HRESULT Run(const std::function<HRESULT()>& Notification);
    bool Invoke(const std::function<HRESULT()>& Work, HRESULT& Result);
    void Stop() noexcept;

private:
    struct Call
    {
        const std::function<HRESULT()>& work;
        wil::unique_event done{wil::EventOptions::ManualReset};
        HRESULT result = E_UNEXPECTED;
    };

    void Drain();

    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::deque<Call*> m_queue;
    _Guarded_by_(m_lock) bool m_stopped = false;
    wil::unique_event m_available{wil::EventOptions::None};
};

} // namespace wsl::windows::service
