/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleStateManager.cpp

Abstract:

    This file contains definitions for coordinating console state leases.

--*/

#include "precomp.h"
#include "ConsoleStateManager.h"

void ConsoleStateManager::Acquire(_In_ HANDLE ConsoleHandle, _Out_ GUID& LeaseId, _Out_ bool& Initialize, _Out_ LXSS_CONSOLE_STATE& ConfiguredState)
{
    ULONG consoleId{};
    wil::unique_handle conhostHandle;
    GetConsoleInfo(ConsoleHandle, consoleId, conhostHandle);
    THROW_IF_FAILED(CoCreateGuid(&LeaseId));

    std::unique_lock lock(m_lock);
    for (;;)
    {
        auto entry = m_entries.find(consoleId);
        if (entry == m_entries.end())
        {
            Entry newEntry;
            newEntry.ConhostHandle = std::move(conhostHandle);
            newEntry.Leases.emplace(LeaseId);
            m_entries.emplace(consoleId, std::move(newEntry));
            Initialize = true;
            ConfiguredState = {};
            return;
        }

        if (entry->second.CurrentState == State::Active)
        {
            entry->second.Leases.emplace(LeaseId);
            Initialize = false;
            ConfiguredState = entry->second.ConfiguredState;
            return;
        }

        m_stateChanged.wait(lock, [&] {
            const auto current = m_entries.find(consoleId);
            return (current == m_entries.end()) || (current->second.CurrentState == State::Active);
        });
    }
}

void ConsoleStateManager::Commit(_In_ const GUID& LeaseId, _In_ const LXSS_CONSOLE_STATE& BaselineState, _In_ const LXSS_CONSOLE_STATE& ConfiguredState)
{
    std::lock_guard lock(m_lock);
    const auto entry = FindLease(LeaseId);
    THROW_HR_IF(E_UNEXPECTED, (entry == m_entries.end()) || (entry->second.CurrentState != State::Initializing));

    entry->second.BaselineState = BaselineState;
    entry->second.ConfiguredState = ConfiguredState;
    entry->second.CurrentState = State::Active;
    m_stateChanged.notify_all();
}

bool ConsoleStateManager::Release(_In_ const GUID& LeaseId, _Out_ LXSS_CONSOLE_STATE& BaselineState, _Out_ LXSS_CONSOLE_STATE& ConfiguredState)
{
    std::lock_guard lock(m_lock);
    const auto entry = FindLease(LeaseId);
    THROW_HR_IF(E_UNEXPECTED, entry == m_entries.end());

    if (entry->second.CurrentState == State::Initializing)
    {
        m_entries.erase(entry);
        m_stateChanged.notify_all();
        return false;
    }

    THROW_HR_IF(E_UNEXPECTED, entry->second.CurrentState != State::Active);
    WI_VERIFY(entry->second.Leases.erase(LeaseId) == 1);
    if (!entry->second.Leases.empty())
    {
        return false;
    }

    entry->second.CurrentState = State::Restoring;
    entry->second.Restorer = LeaseId;
    BaselineState = entry->second.BaselineState;
    ConfiguredState = entry->second.ConfiguredState;
    return true;
}

void ConsoleStateManager::Complete(_In_ const GUID& LeaseId)
{
    std::lock_guard lock(m_lock);
    for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
    {
        if ((entry->second.CurrentState == State::Restoring) && IsEqualGUID(entry->second.Restorer, LeaseId))
        {
            m_entries.erase(entry);
            m_stateChanged.notify_all();
            return;
        }
    }

    THROW_HR(E_UNEXPECTED);
}

ConsoleStateManager::EntryIterator ConsoleStateManager::FindLease(_In_ const GUID& LeaseId)
{
    return std::find_if(
        m_entries.begin(), m_entries.end(), [&](const auto& entry) { return entry.second.Leases.contains(LeaseId); });
}

ULONG ConsoleStateManager::GetConhostServerId(_In_ HANDLE ConsoleHandle)
{
    IO_STATUS_BLOCK ioStatus{};
    HANDLE serverPid{};

    // The IOCTL requires a handle-sized output buffer, but returns a process identifier.
    THROW_IF_NTSTATUS_FAILED(NtDeviceIoControlFile(
        ConsoleHandle, nullptr, nullptr, nullptr, &ioStatus, IOCTL_CONDRV_GET_SERVER_PID, nullptr, 0, &serverPid, sizeof(serverPid)));

    return HandleToUlong(serverPid);
}

void ConsoleStateManager::GetConsoleInfo(_In_ HANDLE ConsoleHandle, _Out_ ULONG& ConsoleId, _Out_ wil::unique_handle& ConhostHandle)
{
    THROW_HR_IF(E_INVALIDARG, !ConsoleHandle || (ConsoleHandle == INVALID_HANDLE_VALUE));

    FILE_FS_DEVICE_INFORMATION deviceInformation{};
    IO_STATUS_BLOCK ioStatus{};
    THROW_IF_NTSTATUS_FAILED(
        NtQueryVolumeInformationFile(ConsoleHandle, &ioStatus, &deviceInformation, sizeof(deviceInformation), FileFsDeviceInformation));
    THROW_HR_IF(E_INVALIDARG, deviceInformation.DeviceType != FILE_DEVICE_CONSOLE);

    ConsoleId = GetConhostServerId(ConsoleHandle);
    ConhostHandle.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ConsoleId));
    THROW_LAST_ERROR_IF(!ConhostHandle);

    // Validate the identifier after opening the process to close the PID-reuse window.
    THROW_HR_IF(E_UNEXPECTED, ConsoleId != GetConhostServerId(ConsoleHandle));
    WI_ASSERT(ConsoleId != 0);
}
