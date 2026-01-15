/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    COMImplClass.cpp

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
        std::lock_guard lock(m_lock);

        WI_ASSERT(m_impl != nullptr);
        m_impl = nullptr;
    }

protected:
    template <typename... Args>
    HRESULT CallImpl(void (TImpl::*routine)(Args... args), Args... args)
    try
    {
        std::lock_guard lock{m_lock};
        RETURN_HR_IF(RPC_E_DISCONNECTED, m_impl == nullptr);

        (m_impl->*routine)(std::forward<Args>(args)...);

        return S_OK;
    }
    CATCH_RETURN();

private:
    std::recursive_mutex m_lock;
    TImpl* m_impl = nullptr;
};

} // namespace wsl::windows::service::wsla