// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windows.h>
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
    class Lock
    {
    public:
        explicit Lock(const std::wstring& Name)
        {
            m_mutex.reset(CreateMutexW(nullptr, FALSE, (L"Global\\WSL.UpgradeGuard." + Name).c_str()));
            THROW_LAST_ERROR_IF(!m_mutex);
            const auto result = WaitForSingleObject(m_mutex.get(), INFINITE);
            THROW_LAST_ERROR_IF(result == WAIT_FAILED);
            THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 && result != WAIT_ABANDONED);
        }

        ~Lock()
        {
            LOG_IF_WIN32_BOOL_FALSE(ReleaseMutex(m_mutex.get()));
        }

        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        wil::unique_handle m_mutex;
    };

public:
    explicit ServiceUpgradeGuard(std::wstring Name) : m_name(std::move(Name)), m_lock(m_name)
    {
        Restore(m_name);
        const auto service = Open(m_name);
        if (!service)
        {
            return;
        }

        const auto startType = GetStartType(service.get());
        if (startType != SERVICE_DISABLED)
        {
            THROW_WIN32_IF(ERROR_INVALID_DATA, startType != SERVICE_AUTO_START && startType != SERVICE_DEMAND_START);
            const auto key = OpenKey(m_name, true);
            THROW_HR_IF(E_UNEXPECTED, !key);
            // Keep recovery outside the service key: MSI can delete that key
            // and then recreate the disabled service during rollback.
            THROW_IF_WIN32_ERROR(
                RegSetValueExW(key.get(), c_originalStart, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&startType), sizeof(startType)));
            THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
            SetStartType(service.get(), SERVICE_DISABLED);
        }
    }

    ServiceUpgradeGuard(const ServiceUpgradeGuard&) = delete;
    ServiceUpgradeGuard& operator=(const ServiceUpgradeGuard&) = delete;

    ~ServiceUpgradeGuard()
    {
        try
        {
            Restore(m_name);
        }
        CATCH_LOG();
    }

    // Call on updater service startup, even if no version upgrade is needed.
    static void Recover(const std::wstring& Name)
    {
        const Lock lock(Name);
        Restore(Name);
    }

private:
    static constexpr auto c_originalStart = L"WslInstallerOriginalStart";

    static wil::unique_hkey OpenKey(const std::wstring& Name, bool Create = false)
    {
        wil::unique_hkey key;
        const auto path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\UpgradeRecovery\\" + Name;
        const auto access = KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY;
        const auto result = Create ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, access, nullptr, key.put(), nullptr)
                                   : RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, access, key.put());
        THROW_WIN32_IF(result, result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND);
        return key;
    }

    static void Restore(const std::wstring& Name)
    {
        const auto key = OpenKey(Name);
        if (!key)
        {
            return;
        }
        DWORD original = 0;
        DWORD type = 0;
        DWORD bytes = sizeof(original);
        const auto result = RegQueryValueExW(key.get(), c_originalStart, nullptr, &type, reinterpret_cast<BYTE*>(&original), &bytes);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            return;
        }
        THROW_IF_WIN32_ERROR(result);
        THROW_WIN32_IF(
            ERROR_INVALID_DATA,
            type != REG_DWORD || bytes != sizeof(original) || (original != SERVICE_AUTO_START && original != SERVICE_DEMAND_START));
        // The MSI server can outlive a crashed updater. Leave the recovery
        // record intact rather than re-enable activation during its execution.
        VerifyInstallerIdle();
        const auto service = Open(Name);
        if (!service)
        {
            return;
        }
        if (GetStartType(service.get()) == SERVICE_DISABLED)
        {
            SetStartType(service.get(), original);
        }
        THROW_IF_WIN32_ERROR(RegDeleteValueW(key.get(), c_originalStart));
        THROW_IF_WIN32_ERROR(RegFlushKey(key.get()));
    }

    static void VerifyInstallerIdle()
    {
        const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        THROW_LAST_ERROR_IF(!manager);
        const wil::unique_schandle service{OpenServiceW(manager.get(), L"msiserver", SERVICE_QUERY_STATUS)};
        if (!service)
        {
            THROW_LAST_ERROR_IF(GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST);
            return;
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        THROW_IF_WIN32_BOOL_FALSE(
            QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes));
        THROW_WIN32_IF(
            ERROR_INSTALL_ALREADY_RUNNING,
            status.dwCurrentState != SERVICE_STOPPED &&
                (status.dwCurrentState != SERVICE_RUNNING || !(status.dwControlsAccepted & SERVICE_ACCEPT_STOP)));
    }

    static wil::unique_schandle Open(const std::wstring& Name)
    {
        const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
        THROW_LAST_ERROR_IF(!manager);
        wil::unique_schandle service{OpenServiceW(manager.get(), Name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG)};
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
    Lock m_lock;
};
