// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windows.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <wil/resource.h>
#include <wil/result.h>

// Experimental protection for upgrades from packages whose uninstall actions do
// not disable the service. Do not retain an SC_HANDLE across MsiInstallProduct:
// that would prevent the old service from being deleted and recreated.
class ServiceUpgradeGuard
{
public:
    explicit ServiceUpgradeGuard(std::wstring Name) : m_name(std::move(Name))
    {
        const auto service = Open();
        if (!service)
        {
            return;
        }

        const auto startType = GetStartType(service.get());
        if (startType != SERVICE_DISABLED)
        {
            SetStartType(service.get(), SERVICE_DISABLED);
            m_originalStartType = startType;
        }
    }

    ServiceUpgradeGuard(const ServiceUpgradeGuard&) = delete;
    ServiceUpgradeGuard& operator=(const ServiceUpgradeGuard&) = delete;

    ~ServiceUpgradeGuard()
    {
        try
        {
            if (m_originalStartType)
            {
                const auto service = Open();
                // Successful installation normally creates an automatic service.
                // Restore only when our temporary disabled state is still present.
                if (service && GetStartType(service.get()) == SERVICE_DISABLED)
                {
                    SetStartType(service.get(), *m_originalStartType);
                }
            }
        }
        CATCH_LOG();
    }

private:
    wil::unique_schandle Open() const
    {
        const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        THROW_LAST_ERROR_IF(!manager);
        wil::unique_schandle service{OpenServiceW(manager.get(), m_name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG)};
        if (!service)
        {
            THROW_LAST_ERROR_IF(GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST);
        }
        return service;
    }

    static DWORD GetStartType(SC_HANDLE Service)
    {
        DWORD bytes = 0;
        QueryServiceConfigW(Service, nullptr, 0, &bytes);
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_INSUFFICIENT_BUFFER);
        std::vector<BYTE> buffer(bytes);
        const auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        THROW_IF_WIN32_BOOL_FALSE(QueryServiceConfigW(Service, config, bytes, &bytes));
        return config->dwStartType;
    }

    static void SetStartType(SC_HANDLE Service, DWORD StartType)
    {
        THROW_IF_WIN32_BOOL_FALSE(ChangeServiceConfigW(
            Service, SERVICE_NO_CHANGE, StartType, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    }

    std::wstring m_name;
    std::optional<DWORD> m_originalStartType;
};
