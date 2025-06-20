/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreAdviseHandler.h

Abstract:

    This file contains WSL Core COM Advise/Unadvise functions.

--*/

#pragma once
#include <vector>
#include <objbase.h>
#include <wil/com.h>
#include <wil/resource.h>

namespace wsl::core {
// This class encapsulates the non-obvious and sometimes non-trivial calls to register for COM callbacks
// using the fairly standardized Advise ConnectionPoint interface
class WslCoreAdviseHandler
{
public:
    WslCoreAdviseHandler() = default;
    ~WslCoreAdviseHandler() = default;

    WslCoreAdviseHandler(const WslCoreAdviseHandler&) = delete;
    WslCoreAdviseHandler& operator=(const WslCoreAdviseHandler&) = delete;
    WslCoreAdviseHandler(WslCoreAdviseHandler&&) = delete;
    WslCoreAdviseHandler& operator=(WslCoreAdviseHandler&&) = delete;

    // typename T is the connection point interface which is implemented
    // typename C is the server object implementing IConnectionPoint to Advise()
    // typename S is the client's sink object (must implement type T)
    template <typename T, typename C, typename S>
    void AdviseInProcObject(wil::com_ptr<C> sourceObject, _In_ S* connectionSink)
    {
        AdviseInstance newInstance;

        const wil::com_ptr<IConnectionPointContainer> pointContainer = sourceObject.template query<IConnectionPointContainer>();
        THROW_IF_FAILED(pointContainer->FindConnectionPoint(__uuidof(T), newInstance.m_connectionPoint.addressof()));
        THROW_IF_FAILED(newInstance.m_connectionPoint->Advise(connectionSink, &newInstance.m_cookie));

        m_adviseInstances.emplace_back(std::move(newInstance));
    }

    // typename C is the instantiated object implementing IConnectionPoint to Advise()
    // typename S is the interface to register with Advise()
    // Also sets the authentication information that will be used to make calls on the specified proxy.
    template <typename T, typename C, typename S>
    void AdviseProxyObject(wil::com_ptr<C> sourceObject, _In_ S* connectionSink)
    {
        AdviseInstance newInstance;

        const wil::com_ptr<IConnectionPointContainer> pointContainer = sourceObject.template query<IConnectionPointContainer>();
        THROW_IF_FAILED(CoSetProxyBlanket(
            pointContainer.get(), RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_NONE, COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_STATIC_CLOAKING));

        THROW_IF_FAILED(pointContainer->FindConnectionPoint(__uuidof(T), newInstance.m_connectionPoint.addressof()));
        THROW_IF_FAILED(CoSetProxyBlanket(
            newInstance.m_connectionPoint.get(), RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_NONE, COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_STATIC_CLOAKING));

        THROW_IF_FAILED(newInstance.m_connectionPoint->Advise(static_cast<IUnknown*>(connectionSink), &newInstance.m_cookie));
        m_adviseInstances.emplace_back(std::move(newInstance));
    }

    void Reset() noexcept
    {
        m_adviseInstances.clear();
    }

private:
    struct AdviseInstance
    {
        wil::com_ptr<IConnectionPoint> m_connectionPoint{};
        DWORD m_cookie{0};

        AdviseInstance() noexcept = default;
        ~AdviseInstance() noexcept
        {
            if (m_connectionPoint && m_cookie != 0)
            {
                m_connectionPoint->Unadvise(m_cookie);
            }
        }

        AdviseInstance(const AdviseInstance&) = delete;
        AdviseInstance& operator=(const AdviseInstance&) = delete;
        AdviseInstance(AdviseInstance&&) = default;
        AdviseInstance& operator=(AdviseInstance&&) = default;
    };

    std::vector<AdviseInstance> m_adviseInstances;
};
} // namespace wsl::core