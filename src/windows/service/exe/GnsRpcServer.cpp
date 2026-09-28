// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "GnsRpcServer.h"
#include "wsldeps.h"

static constexpr PCWSTR c_SystemOnlySDDL =
    // Owner: System
    L"O:SY"
    // Group: System
    L"G:SY"
    // Begin DACL
    L"D:"
    // Allow System full access
    L"(A;;GA;;;SY)";

static std::weak_ptr<GnsRpcServer> instance;
wil::unique_hlocal_security_descriptor systemOnlySD;

std::shared_ptr<GnsRpcServer> GnsRpcServer::GetOrCreate()
{
    static wil::srwlock instanceLock;
    auto lock = instanceLock.lock_exclusive();

    if (nullptr == systemOnlySD)
    {
        THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptor(c_SystemOnlySDDL, SDDL_REVISION_1, &systemOnlySD, nullptr));
    }

    auto ptr = instance.lock();
    if (!ptr)
    {
        ptr = std::make_shared<GnsRpcServer>();
        instance = ptr;
    }

    return ptr;
}

GnsRpcServer::GnsRpcServer()
{
    THROW_IF_FAILED(WslDepsRegisterGnsRpcServer(systemOnlySD.get(), &m_serverUuid));
}

GnsRpcServer::~GnsRpcServer() noexcept
{
    if (!m_moved)
    {
        LOG_IF_FAILED(WslDepsUnregisterGnsRpcServer(&m_serverUuid));
    }
}

GnsRpcServer::GnsRpcServer(GnsRpcServer&& Other) noexcept
{
    *this = std::move(Other);
}

GnsRpcServer& GnsRpcServer::operator=(GnsRpcServer&& Other) noexcept
{
    m_serverUuid = Other.m_serverUuid;
    Other.m_serverUuid = GUID{};
    Other.m_moved = true;

    return *this;
}

const UUID& GnsRpcServer::GetServerUuid() const noexcept
{
    return m_serverUuid;
}
