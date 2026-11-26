// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "InstallGuard.h"
#include <charconv>

bool wsl::windows::common::install::IsServiceBusy()
{
    wil::unique_event event;
    if (!event.try_open(c_serviceBusyEventName, SYNCHRONIZE | READ_CONTROL))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_FILE_NOT_FOUND);
        return false;
    }

    THROW_HR_IF(E_ACCESSDENIED, !IsOwnedByService(event.get()));

    // The event is signaled while the service is busy.
    const auto result = WaitForSingleObject(event.get(), 0);
    THROW_LAST_ERROR_IF(result == WAIT_FAILED);
    THROW_HR_IF(E_UNEXPECTED, result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT);
    return result == WAIT_OBJECT_0;
}

bool wsl::windows::common::install::IsExplicitInstallClient(std::wstring_view clientProcessId)
{
    // Parse the PID and find the client process
    const auto pidString = wsl::windows::common::string::WideToMultiByte(clientProcessId);
    DWORD pid{};
    const auto result = std::from_chars(pidString.data(), pidString.data() + pidString.size(), pid);
    THROW_HR_IF(E_INVALIDARG, result.ec != std::errc{} || result.ptr != pidString.data() + pidString.size() || pid == 0);

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

bool wsl::windows::common::install::IsOwnedByService(HANDLE handle)
{
    auto [serviceSid, serviceSidBuffer] = wsl::windows::common::security::GetAccountSid(L"NT SERVICE\\WslService");
    wil::unique_hlocal_security_descriptor descriptor;
    PSID owner{};
    THROW_IF_WIN32_ERROR(GetSecurityInfo(
        handle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor));
    return owner && EqualSid(owner, serviceSid);
}
