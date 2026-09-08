// Copyright (C) Microsoft Corporation. All rights reserved.
// Isolated contract tests: no real service configuration is changed.
#include <windows.h>
#include <optional>
namespace {
int openHandles;
BOOL WINAPI CloseHandleForTest(SC_HANDLE)
{
    --openHandles;
    return TRUE;
}
LSTATUS WINAPI CloseKeyForTest(HKEY)
{
    return ERROR_SUCCESS;
}
} // namespace
#define CloseServiceHandle CloseHandleForTest
#define RegCloseKey CloseKeyForTest
#include <wil/resource.h>
#undef CloseServiceHandle
#undef RegCloseKey
#include <wil/result.h>
#include <cstdio>
#include <stdexcept>

namespace {
DWORD startType;
bool exists;
bool failChange;
int writes;
std::optional<DWORD> journal;
bool failFlush;
bool installerBusy;

BOOL WINAPI QueryStatus(SC_HANDLE, SC_STATUS_TYPE, LPBYTE Buffer, DWORD, LPDWORD Size)
{
    auto* status = reinterpret_cast<SERVICE_STATUS_PROCESS*>(Buffer);
    *status = {};
    status->dwCurrentState = installerBusy ? SERVICE_RUNNING : SERVICE_STOPPED;
    *Size = sizeof(*status);
    return TRUE;
}

LSTATUS WINAPI RegistryOpenForTest(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY Key)
{
    *Key = reinterpret_cast<HKEY>(3);
    return ERROR_SUCCESS;
}
LSTATUS WINAPI CreateKeyForTest(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY Key, LPDWORD)
{
    *Key = reinterpret_cast<HKEY>(3);
    return ERROR_SUCCESS;
}
LSTATUS WINAPI QueryValue(HKEY, LPCWSTR, LPDWORD, LPDWORD Type, LPBYTE Value, LPDWORD Size)
{
    if (!journal)
    {
        return ERROR_FILE_NOT_FOUND;
    }
    *Type = REG_DWORD;
    *Size = sizeof(DWORD);
    *reinterpret_cast<DWORD*>(Value) = *journal;
    return ERROR_SUCCESS;
}
LSTATUS WINAPI SetValue(HKEY, LPCWSTR, DWORD, DWORD, const BYTE* Value, DWORD)
{
    journal = *reinterpret_cast<const DWORD*>(Value);
    return ERROR_SUCCESS;
}
LSTATUS WINAPI DeleteValue(HKEY, LPCWSTR)
{
    journal.reset();
    return ERROR_SUCCESS;
}
LSTATUS WINAPI FlushKey(HKEY)
{
    return failFlush ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
}

SC_HANDLE WINAPI OpenManager(LPCWSTR, LPCWSTR, DWORD)
{
    ++openHandles;
    return reinterpret_cast<SC_HANDLE>(1);
}
SC_HANDLE WINAPI OpenService(SC_HANDLE, LPCWSTR, DWORD)
{
    if (!exists)
    {
        SetLastError(ERROR_SERVICE_DOES_NOT_EXIST);
        return nullptr;
    }
    ++openHandles;
    return reinterpret_cast<SC_HANDLE>(2);
}
BOOL WINAPI QueryConfig(SC_HANDLE, LPQUERY_SERVICE_CONFIGW Config, DWORD Size, LPDWORD Needed)
{
    *Needed = sizeof(QUERY_SERVICE_CONFIGW);
    if (Size < *Needed)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    Config->dwStartType = startType;
    return TRUE;
}
BOOL WINAPI ChangeConfig(SC_HANDLE, DWORD, DWORD Start, DWORD, LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR)
{
    if (failChange)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    ++writes;
    startType = Start;
    return TRUE;
}
void Check(bool Condition)
{
    if (!Condition)
    {
        throw std::runtime_error("guard contract failed");
    }
}
void Reset(DWORD Start = SERVICE_AUTO_START)
{
    Check(openHandles == 0);
    startType = Start;
    exists = true;
    failChange = false;
    writes = 0;
    journal.reset();
    failFlush = false;
    installerBusy = false;
}
} // namespace

// Replace the SCM operations used by the guard, including handle cleanup.
#define OpenSCManagerW OpenManager
#define OpenServiceW OpenService
#define QueryServiceConfigW QueryConfig
#define ChangeServiceConfigW ChangeConfig
#define RegOpenKeyExW RegistryOpenForTest
#define RegCreateKeyExW CreateKeyForTest
#define RegQueryValueExW QueryValue
#define RegSetValueExW SetValue
#define RegDeleteValueW DeleteValue
#define RegFlushKey FlushKey
#define QueryServiceStatusEx QueryStatus
#include "ServiceUpgradeGuard.h"
#undef OpenSCManagerW
#undef OpenServiceW
#undef QueryServiceConfigW
#undef ChangeServiceConfigW
#undef RegOpenKeyExW
#undef RegCreateKeyExW
#undef RegQueryValueExW
#undef RegSetValueExW
#undef RegDeleteValueW
#undef RegFlushKey
#undef QueryServiceStatusEx

int main()
{
    try
    {
        for (const auto original : {SERVICE_AUTO_START, SERVICE_DEMAND_START})
        {
            Reset(original);
            {
                ServiceUpgradeGuard guard(L"test");
                Check(startType == SERVICE_DISABLED && openHandles == 0);
            }
            Check(startType == original && writes == 2);
        }
        Reset();
        try
        {
            ServiceUpgradeGuard guard(L"test");
            throw std::runtime_error("simulated installer failure");
        }
        catch (const std::runtime_error&)
        {
        }
        Check(startType == SERVICE_AUTO_START);

        Reset(SERVICE_DISABLED);
        {
            ServiceUpgradeGuard guard(L"test");
        }
        Check(startType == SERVICE_DISABLED && writes == 0);

        Reset();
        {
            ServiceUpgradeGuard guard(L"test");
            startType = SERVICE_DEMAND_START;
        }
        Check(startType == SERVICE_DEMAND_START && writes == 1);

        Reset();
        exists = false;
        {
            ServiceUpgradeGuard guard(L"test");
        }
        Check(writes == 0);

        Reset();
        {
            ServiceUpgradeGuard guard(L"test");
            exists = false;
        }
        Check(writes == 1);

        Reset();
        failChange = true;
        bool failed = false;
        try
        {
            ServiceUpgradeGuard guard(L"test");
        }
        catch (...)
        {
            failed = true;
        }
        Check(failed && writes == 0 && startType == SERVICE_AUTO_START && openHandles == 0);
        failChange = false;
        Check(ServiceUpgradeGuard::Recover(L"test") == S_OK);
        Check(!journal && startType == SERVICE_AUTO_START);

        Reset(SERVICE_DISABLED);
        journal = SERVICE_DEMAND_START;
        Check(ServiceUpgradeGuard::Recover(L"test") == S_OK);
        Check(startType == SERVICE_DEMAND_START && !journal);

        Reset();
        failFlush = true;
        failed = false;
        try
        {
            ServiceUpgradeGuard guard(L"test");
        }
        catch (...)
        {
            failed = true;
        }
        Check(failed && writes == 0 && startType == SERVICE_AUTO_START);

        Reset(SERVICE_DISABLED);
        journal = SERVICE_BOOT_START;
        Check(ServiceUpgradeGuard::Recover(L"test") == HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        Check(writes == 0 && startType == SERVICE_DISABLED && journal == SERVICE_BOOT_START && openHandles == 0);

        Reset(SERVICE_DISABLED);
        journal = SERVICE_AUTO_START;
        installerBusy = true;
        Check(ServiceUpgradeGuard::Recover(L"test") == HRESULT_FROM_WIN32(ERROR_INSTALL_ALREADY_RUNNING));
        Check(startType == SERVICE_DISABLED && journal == SERVICE_AUTO_START && openHandles == 0);
        // A new upgrade must still fail closed while recovery is deferred.
        failed = false;
        try
        {
            ServiceUpgradeGuard guard(L"test");
        }
        catch (...)
        {
            failed = true;
        }
        Check(failed && writes == 0 && startType == SERVICE_DISABLED && journal == SERVICE_AUTO_START);
        installerBusy = false;
        Check(ServiceUpgradeGuard::Recover(L"test") == S_OK);
        Check(startType == SERVICE_AUTO_START && !journal);

        Reset(SERVICE_DISABLED);
        journal = SERVICE_DEMAND_START;
        failChange = true;
        Check(ServiceUpgradeGuard::Recover(L"test") == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
        Check(startType == SERVICE_DISABLED && journal == SERVICE_DEMAND_START && openHandles == 0);
        failChange = false;
        Check(ServiceUpgradeGuard::Recover(L"test") == S_OK);
        Check(startType == SERVICE_DEMAND_START && !journal);
        Reset();
        {
            ServiceUpgradeGuard guard(L"test");
            // MSI removes the service and rolls back its disabled configuration.
            exists = false;
            Check(journal.has_value());
            exists = true;
            startType = SERVICE_DISABLED;
        }
        Check(startType == SERVICE_AUTO_START && !journal);
        std::puts("PASS: automatic/manual restore, exception, disabled, replaced, absent, deleted, access denied");
        return 0;
    }
    catch (...)
    {
        std::puts("FAIL");
        return 1;
    }
}
