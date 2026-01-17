/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    COMImplClass.h

Abstract:

    This file contains the definition for COMImplClass, a helper to forward calls from a COM class to an impl class.

--*/

#pragma once

namespace wsl::windows::service::wsla {

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

    auto LockImpl()
    {
        std::unique_lock lock{m_lock};
        THROW_HR_IF(RPC_E_DISCONNECTED, m_impl == nullptr);

        return std::make_pair(std::move(lock), m_impl);
    }

private:
    std::recursive_mutex m_lock;
    TImpl* m_impl = nullptr;
};

} // namespace wsl::windows::service::wsla