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

template <typename TImpl, typename TPointer = TImpl*>
class COMImplClass
{
public:
    // Stores the pointer to the underlying impl. Kept separate from the constructor so that the
    // std::weak_ptr specialization can be initialized after the impl is owned by a std::shared_ptr
    // (e.g. via std::enable_shared_from_this::weak_from_this()).
    void Initialize(TPointer impl)
    {
        m_impl = std::move(impl);
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

        ResetImpl();
    }

protected:
    template <typename... Args>
    HRESULT CallImpl(void (TImpl::*routine)(Args... args), Args... args)
    try
    {
        auto [lock, impl] = LockImpl();
        ((*impl).*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    template <typename... Args>
    HRESULT CallImpl(void (TImpl::*routine)(Args... args) const, Args... args)
    try
    {
        auto [lock, impl] = LockImpl();
        ((*impl).*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

    // Returns a usable pointer to the impl for the current call: the raw pointer for the TImpl*
    // specialization, or a strong std::shared_ptr locked from the weak_ptr otherwise.
    // Must be called with m_lock held.
    auto GetPointer()
    {
        if constexpr (std::is_same_v<TPointer, TImpl*>)
        {
            return m_impl;
        }
        else
        {
            return m_impl.lock();
        }
    }

    [[nodiscard]] auto LockImpl()
    {
        // Obtain a usable reference to the impl and add ourselves to the list of callers.
        // For the std::weak_ptr specialization, GetPointer() returns a strong std::shared_ptr that
        // keeps the impl alive for the full duration of the call, even if the owning container map
        // entry is erased concurrently on another thread.
        auto impl = [this] {
            std::unique_lock lock{m_lock};

            auto pointer = GetPointer();
            THROW_HR_IF(RPC_E_DISCONNECTED, !pointer);

            auto [_, inserted] = m_callers.insert(std::this_thread::get_id());
            WI_ASSERT(inserted);

            return pointer;
        }();

        auto release = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() {
            std::unique_lock lock{m_lock};

            auto removed = m_callers.erase(std::this_thread::get_id());
            WI_ASSERT(removed == 1);

            m_cv.notify_one();
        });

        return std::make_pair(std::move(release), std::move(impl));
    }

private:
    // Clears the impl pointer so subsequent LockImpl() calls fail with RPC_E_DISCONNECTED.
    // Must be called with m_lock held.
    void ResetImpl() noexcept
    {
        if constexpr (std::is_same_v<TPointer, TImpl*>)
        {
            m_impl = nullptr;
        }
        else
        {
            m_impl.reset();
        }
    }

    std::mutex m_lock;
    std::condition_variable m_cv;
    _Guarded_by_(m_lock) std::unordered_set<std::thread::id> m_callers;
    TPointer m_impl{};
};

} // namespace wsl::windows::service::wslc