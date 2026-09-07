// Copyright (C) Microsoft Corporation. All rights reserved.
// Isolated contract tests: no real service configuration is changed.
#include <windows.h>
namespace {
int openHandles;
BOOL WINAPI CloseHandleForTest(SC_HANDLE)
{
    --openHandles;
    return TRUE;
}
} // namespace
#define CloseServiceHandle CloseHandleForTest
#include <wil/resource.h>
#undef CloseServiceHandle
#include <wil/result.h>
#include <cstdio>
#include <stdexcept>

namespace {
DWORD startType;
bool exists;
bool failChange;
int writes;

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
}
} // namespace

// Replace the SCM operations used by the guard, including handle cleanup.
#define OpenSCManagerW OpenManager
#define OpenServiceW OpenService
#define QueryServiceConfigW QueryConfig
#define ChangeServiceConfigW ChangeConfig
#include "ServiceUpgradeGuard.h"
#undef OpenSCManagerW
#undef OpenServiceW
#undef QueryServiceConfigW
#undef ChangeServiceConfigW

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
        std::puts("PASS: automatic/manual restore, exception, disabled, replaced, absent, deleted, access denied");
        return 0;
    }
    catch (...)
    {
        std::puts("FAIL");
        return 1;
    }
}
