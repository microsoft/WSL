// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "InstallGuard.h"
#include <charconv>

bool wsl::windows::common::install::IsServiceBusy()
{
    // Open the mutex that signals whether the service is busy.
    wil::unique_mutex mutex;
    if (!mutex.try_open(c_serviceBusyMutexName, SYNCHRONIZE | READ_CONTROL))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_FILE_NOT_FOUND);
        return false;
    }

    // The service's mutex is owned by System. Do not trust anything else.
    THROW_HR_IF(E_ACCESSDENIED, !IsOwnedBySystem(mutex.get()));

    // When the service is busy, it holds the mutex, so a wait on it will time out.
    // Note that this may be inaccurate if the service failed to acquire the mutex
    // or an unrelated process is holding it.
    const auto result = WaitForSingleObject(mutex.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    if (result == WAIT_TIMEOUT)
    {
        return true;
    }

    THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 && result != WAIT_ABANDONED);

    LOG_IF_WIN32_BOOL_FALSE(ReleaseMutex(mutex.get()));
    return false;
}

bool wsl::windows::common::install::IsExplicitInstallClient(std::wstring_view clientProcessId)
{
    // Parse the PID and find the client process
    const auto pidString = wsl::windows::common::string::WideToMultiByte(clientProcessId);
    DWORD pid{};
    const auto result = std::from_chars(pidString.data(), pidString.data() + pidString.size(), pid);
    if (result.ec != std::errc{} || result.ptr != pidString.data() + pidString.size() || pid == 0)
    {
        return false;
    }

    const wil::unique_process_handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process)
    {
        return false;
    }

    // Check if the process is msiexec or wsl.
    // TODO: Consider if we need additional validation. For example, checking for a MS signature,
    // a specific location, or the original file name.
    std::wstring path;
    THROW_IF_FAILED(wil::QueryFullProcessImageNameW(process.get(), 0, path));
    const auto name = std::filesystem::path(path).filename().wstring();
    return _wcsicmp(name.c_str(), L"msiexec.exe") == 0 || _wcsicmp(name.c_str(), L"wsl.exe") == 0;
}

bool wsl::windows::common::install::IsOwnedBySystem(HANDLE handle)
{
    wil::unique_hlocal_security_descriptor descriptor;
    PSID owner{};
    THROW_IF_WIN32_ERROR(GetSecurityInfo(handle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor));
    return owner && IsWellKnownSid(owner, WinLocalSystemSid);
}
