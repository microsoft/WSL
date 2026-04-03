/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    COMImplClass.h

Abstract:

    This file contains the definition for COMImplClass, a helper to forward calls from a COM class to an impl class.
    // N.B. This class allows multiple calls to happen in parallel, and only blocks Disconnect() until there are either no more callers, or only calling thread is in a call to the underlying class.
    // This is implemented that way so a caller can call Disconnect() from within a call to the COM class without causing a deadlock.

--*/

#pragma once

namespace wsl::windows::service::wslc {

template <typename TImpl>
class COMImplClass
{
public:
    COMImplClass(TImpl* impl) : m_impl(impl)
    {
    }

    void Disconnect() noexcept
    {
        std::unique_lock lock(m_lock);

        // Only continue if either:
        // - There are no current callers
        // - This thread is the only caller

        m_cv.wait(lock, [this] {
            return m_callers.empty() || m_callers.size() == 1 && *m_callers.begin() == std::this_thread::get_id();
        });

        WI_ASSERT(m_impl != nullptr);
        m_impl = nullptr;
    }

protected:
    template <typename... Args>
    HRESULT CallImpl(void (TImpl::*routine)(Args... args), Args... args)
    try
    {
        auto [lock, impl] = LockImpl();
        (impl->*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    template <typename... Args>
    HRESULT CallImpl(void (TImpl::*routine)(Args... args) const, Args... args)
    try
    {
        auto [lock, impl] = LockImpl();
        (impl->*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    [[nodiscard]] auto LockImpl()
    {
        // Check if m_impl is available and add ourselves to the list of callers if that's the case.
        {
            std::unique_lock lock{m_lock};
            THROW_HR_IF(RPC_E_DISCONNECTED, m_impl == nullptr);

            auto [_, inserted] = m_callers.insert(std::this_thread::get_id());
            WI_ASSERT(inserted);
        }

        auto release = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() {
            std::unique_lock lock{m_lock};

            auto removed = m_callers.erase(std::this_thread::get_id());
            WI_ASSERT(removed == 1);

            m_cv.notify_one();
        });

        return std::make_pair(std::move(release), m_impl);
    }

private:
    std::mutex m_lock;
    std::condition_variable m_cv;
    _Guarded_by_(m_lock) std::unordered_set<std::thread::id> m_callers;
    TImpl* m_impl = nullptr;
};

} // namespace wsl::windows::service::wslc