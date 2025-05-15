/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Redirector.cpp

Abstract:

    This file contains helpers for controlling the Plan 9 Redirector.

--*/

#include "precomp.h"
#include <p9rdr.h>
#include <afunix.h>
#include "Redirector.h"

namespace {
struct ConnectionSecurityContext
{
    LUID LogonId;
    LUID LinkedLogonId;
};

ConnectionSecurityContext GetUserLogonIds(_In_ HANDLE token)
{
    const auto tokenGroups = wil::get_token_information<TOKEN_GROUPS_AND_PRIVILEGES>(token);

    // Try to get the linked token. If that fails, just use the one token.
    wil::unique_token_linked_token tokenInfo{};
    if (FAILED(wil::get_token_information_nothrow(tokenInfo, token)))
    {
        return {tokenGroups->AuthenticationId, tokenGroups->AuthenticationId};
    }

    const auto linkedTokenGroups = wil::get_token_information<TOKEN_GROUPS_AND_PRIVILEGES>(tokenInfo.LinkedToken);
    return {tokenGroups->AuthenticationId, linkedTokenGroups->AuthenticationId};
}
} // namespace

namespace wsl::windows::common::redirector {

constexpr wchar_t c_RedirectorServiceName[] = L"P9Rdr";

// Removes all connection targets.
void ClearConnectionTargets(HANDLE device)
{
    filesystem::DeviceIoControl(device, IOCTL_P9RDR_CLEAR_CONNECTION_TARGETS);
}

// Opens the device object for the Plan 9 redirector.
wil::unique_hfile OpenRedirector()
{
    UNICODE_STRING name{};
    RtlInitUnicodeString(&name, P9RDR_DEVICE_NAME);
    return filesystem::OpenRelativeFile(nullptr, &name, GENERIC_READ, FILE_OPEN, 0);
}

// Starts the Plan 9 mini-redirector.
bool StartRedirector(HANDLE device)
{
    const NTSTATUS status = filesystem::DeviceIoControlNoThrow(device, IOCTL_P9RDR_START);
    if (!NT_SUCCESS(status))
    {
        if (status != STATUS_REDIRECTOR_STARTED)
        {
            THROW_NTSTATUS(status);
        }

        return false;
    }

    return true;
}

// Starts the Plan 9 mini-redirector.
bool StartRedirector()
{
    const auto rdr = OpenRedirector();
    return StartRedirector(rdr.get());
}

// Starts the Plan 9 redirector system service.
bool StartRedirectorService()
{
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT)};
    THROW_LAST_ERROR_IF(!manager);

    const wil::unique_schandle service{OpenService(manager.get(), c_RedirectorServiceName, SERVICE_START)};
    THROW_LAST_ERROR_IF(!service);
    if (!StartService(service.get(), 0, nullptr))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_SERVICE_ALREADY_RUNNING);
        return false;
    }

    return true;
}

// Make sure the Plan 9 Redirector device is present, the mini-redirector is started, and is in a
// clean state.
void EnsureRedirectorStarted()
{
    const bool serviceStarted = StartRedirectorService();
    const auto rdr = OpenRedirector();

    // Clear any connection targets that may be left over e.g. if the WSL service crashed
    // before.
    // N.B. This isn't necessary if the redirector service was just started.
    if (!serviceStarted)
    {
        ClearConnectionTargets(rdr.get());
    }

    // Always send the start ioctl, because even if the service was running this might not have been
    // sent before.
    StartRedirector(rdr.get());
}

// Adds a connection target to the Plan 9 Redirector.
void AddConnectionTarget(std::wstring_view name, LUID logonId, std::string_view aname, LX_UID_T uid, std::wstring_view unixSocketPath, const GUID& instanceId, ULONG port)
{
    const auto nameBytes = as_bytes(gsl::make_span(name.data(), name.size()));
    const auto anameBytes = as_bytes(gsl::make_span(aname.data(), aname.size()));
    const auto unixSocketPathBytes = as_bytes(gsl::make_span(unixSocketPath.data(), unixSocketPath.size()));

    const auto size = sizeof(P9RDR_ADD_CONNECTION_TARGET_INPUT) + nameBytes.size() + anameBytes.size() + unixSocketPathBytes.size();
    std::vector<gsl::byte> buffer(size);

    const auto addConnection = gslhelpers::get_struct<P9RDR_ADD_CONNECTION_TARGET_INPUT>(gsl::make_span(buffer));
    if (!unixSocketPathBytes.empty())
    {
        // This is regular WSL, which uses a Unix socket.
        const auto unixAddress = reinterpret_cast<PSOCKADDR_UN>(&addConnection->Address);

        // The path in the sockaddr_un is not used, but it should not be empty. Just put the
        // unqualified file name in there.
        unixAddress->sun_family = AF_UNIX;
        strcpy_s(unixAddress->sun_path, LXSS_PLAN9_UNIX_SOCKET_A);
    }
    else
    {
        // This is a VM mode instance, so use a Hyper-V socket.
        const auto hvAddress = reinterpret_cast<PSOCKADDR_HV>(&addConnection->Address);
        hvAddress->Family = AF_HYPERV;
        hvAddress->VmId = instanceId;
        hvAddress->ServiceId = HV_GUID_VSOCK_TEMPLATE;
        hvAddress->ServiceId.Data1 = port;
    }

    addConnection->Uid = uid;
    addConnection->LogonId = logonId;
    addConnection->ShareNameLength = gsl::narrow_cast<USHORT>(name.length() * sizeof(wchar_t));
    addConnection->ANameLength = gsl::narrow_cast<USHORT>(aname.length());

    // Copy over the share name.
    auto stringSpan = gsl::make_span(buffer).subspan(sizeof(P9RDR_ADD_CONNECTION_TARGET_INPUT));
    gsl::copy(nameBytes, stringSpan);
    stringSpan = stringSpan.subspan(nameBytes.size());

    // Copy over the aname.
    if (aname.size() > 0)
    {
        gsl::copy(anameBytes, stringSpan);
        stringSpan = stringSpan.subspan(anameBytes.size());
    }

    // Copy over the unix socket path.
    if (unixSocketPathBytes.size() > 0)
    {
        gsl::copy(unixSocketPathBytes, stringSpan);
    }

    // Send the command to the driver.
    const auto rdr = OpenRedirector();
    filesystem::DeviceIoControl(rdr.get(), IOCTL_P9RDR_ADD_CONNECTION_TARGET, buffer);
}

// Removes a connection target from the Plan 9 Redirector.
void RemoveConnectionTarget(std::wstring_view name, LUID logonId)
{
    const auto nameBytes = as_bytes(gsl::make_span(name.data(), name.length()));
    std::vector<gsl::byte> buffer(sizeof(P9RDR_REMOVE_CONNECTION_TARGET_INPUT) + nameBytes.size());

    const auto removeConnection = gslhelpers::get_struct<P9RDR_REMOVE_CONNECTION_TARGET_INPUT>(gsl::make_span(buffer));
    removeConnection->LogonId = logonId;

    // Copy over the share name.
    const auto stringSpan = gsl::make_span(buffer).subspan(sizeof(P9RDR_REMOVE_CONNECTION_TARGET_INPUT));
    gsl::copy(nameBytes, stringSpan);

    // Send the command to the driver.
    const auto rdr = OpenRedirector();
    const NTSTATUS status = filesystem::DeviceIoControlNoThrow(rdr.get(), IOCTL_P9RDR_REMOVE_CONNECTION_TARGET, buffer);

    // If the share didn't exist, that's weird but not a failure.
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
    {
        THROW_NTSTATUS(status);
    }
}

// Registers a user-mode callback with the Plan 9 Redirector.
void RegisterUserCallback(HANDLE handle, gsl::span<gsl::byte> outputBuffer, LPOVERLAPPED overlapped)
{
    if (!DeviceIoControl(
            handle, IOCTL_P9RDR_REGISTER_USER_CALLBACK, nullptr, 0, outputBuffer.data(), gsl::narrow_cast<DWORD>(outputBuffer.size()), nullptr, overlapped))
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
    }
}

ConnectionTargetManager::ConnectionTargetManager(std::wstring_view name) : m_name{name}
{
}

// Registers connection targets for the specified logon ID and linked logon ID, if they're not
// already registered.
void ConnectionTargetManager::AddConnectionTarget(
    HANDLE userToken, std::string_view aname, LX_UID_T uid, std::wstring_view unixSocketPath, const GUID& instanceId, ULONG port)
{
    const auto security = GetUserLogonIds(userToken);
    auto lock = m_lock.lock_exclusive();
    if (!Contains(security.LogonId))
    {
        redirector::AddConnectionTarget(m_name, security.LogonId, aname, uid, unixSocketPath, instanceId, port);
        m_logonIds.push_back(security.LogonId);
    }

    // Checking the list also catches the case where the logon ID and linked logon ID are equal.
    if (!Contains(security.LinkedLogonId))
    {
        redirector::AddConnectionTarget(m_name, security.LinkedLogonId, aname, uid, unixSocketPath, instanceId, port);
        m_logonIds.push_back(security.LinkedLogonId);
    }
}

// Removes all connection targets associated with the instance.
void ConnectionTargetManager::RemoveAll()
{
    auto lock = m_lock.lock_exclusive();
    for (const auto logonId : m_logonIds)
    {
        RemoveConnectionTarget(m_name, logonId);
    }

    m_logonIds.clear();
}

// Checks whether the list of logon IDs contains the specified ID.
bool ConnectionTargetManager::Contains(LUID luid) const
{
    const auto it = std::find_if(m_logonIds.begin(), m_logonIds.end(), [&luid](auto& item) { return RtlEqualLuid(&luid, &item); });

    return it != m_logonIds.end();
}

} // namespace wsl::windows::common::redirector
