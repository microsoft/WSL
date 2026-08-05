/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleStateManager.cpp

Abstract:

    This file contains definitions for coordinating console state leases.

--*/

#include "precomp.h"
#include "ConsoleStateManager.h"

void ConsoleStateManager::Acquire(_In_ HANDLE ConsoleHandle, _In_ HANDLE ClientProcess, _Out_ GUID& LeaseId, _Out_ bool& Initialize, _Out_ LXSS_CONSOLE_STATE& ConfiguredState)
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
            const auto [newEntryIterator, inserted] = m_entries.emplace(consoleId, std::move(newEntry));
            WI_ASSERT(inserted);
            auto removeEntry = wil::scope_exit([&] { m_entries.erase(newEntryIterator); });
            AddLease(newEntryIterator->second, LeaseId, ClientProcess);
            removeEntry.release();
            Initialize = true;
            ConfiguredState = {};
            return;
        }

        if (entry->second.CurrentState == State::Active)
        {
            AddLease(entry->second, LeaseId, ClientProcess);
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
    if ((entry == m_entries.end()) || (entry->second.CurrentState != State::Initializing))
    {
        return;
    }

    entry->second.BaselineState = BaselineState;
    entry->second.ConfiguredState = ConfiguredState;
    entry->second.CurrentState = State::Active;
    m_stateChanged.notify_all();
}

bool ConsoleStateManager::Release(_In_ const GUID& LeaseId, _Out_ LXSS_CONSOLE_STATE& BaselineState, _Out_ LXSS_CONSOLE_STATE& ConfiguredState)
{
    BaselineState = {};
    ConfiguredState = {};

    std::vector<ULONG64> callbackIds;
    bool restore = false;
    {
        std::lock_guard lock(m_lock);
        const auto entry = FindLease(LeaseId);
        if (entry == m_entries.end())
        {
            return false;
        }

        const auto lease = entry->second.Leases.find(LeaseId);
        WI_ASSERT(lease != entry->second.Leases.end());

        if (entry->second.CurrentState == State::Initializing)
        {
            callbackIds.emplace_back(lease->second.CallbackId);
            m_entries.erase(entry);
            m_stateChanged.notify_all();
        }
        else if (entry->second.CurrentState == State::Active)
        {
            for (auto otherLease = entry->second.Leases.begin(); otherLease != entry->second.Leases.end();)
            {
                if (!IsEqualGUID(otherLease->first, LeaseId) && (WaitForSingleObject(otherLease->second.ClientProcess.get(), 0) == WAIT_OBJECT_0))
                {
                    callbackIds.emplace_back(otherLease->second.CallbackId);
                    otherLease = entry->second.Leases.erase(otherLease);
                }
                else
                {
                    ++otherLease;
                }
            }

            if (entry->second.Leases.size() > 1)
            {
                callbackIds.emplace_back(lease->second.CallbackId);
                entry->second.Leases.erase(lease);
            }
            else
            {
                entry->second.CurrentState = State::Restoring;
                entry->second.Restorer = LeaseId;
                BaselineState = entry->second.BaselineState;
                ConfiguredState = entry->second.ConfiguredState;
                restore = true;
            }
        }
    }

    for (const auto callbackId : callbackIds)
    {
        // A false result means the process-termination callback won the race.
        (void)m_lifetimeManager.RemoveCallback(callbackId);
    }

    return restore;
}

void ConsoleStateManager::Complete(_In_ const GUID& LeaseId)
{
    ULONG64 callbackId{};
    {
        std::lock_guard lock(m_lock);
        const auto entry = FindLease(LeaseId);
        if ((entry == m_entries.end()) || (entry->second.CurrentState != State::Restoring) || !IsEqualGUID(entry->second.Restorer, LeaseId))
        {
            return;
        }

        callbackId = entry->second.Leases.at(LeaseId).CallbackId;
        m_entries.erase(entry);
        m_stateChanged.notify_all();
    }

    // A false result means the process-termination callback won the race.
    (void)m_lifetimeManager.RemoveCallback(callbackId);
}

ConsoleStateManager::EntryIterator ConsoleStateManager::FindLease(_In_ const GUID& LeaseId)
{
    return std::find_if(
        m_entries.begin(), m_entries.end(), [&](const auto& entry) { return entry.second.Leases.contains(LeaseId); });
}

void ConsoleStateManager::AddLease(_Inout_ Entry& Entry, _In_ const GUID& LeaseId, _In_ HANDLE ClientProcess)
{
    const auto callbackId = m_lifetimeManager.GetRegistrationId();
    Lease lease{wil::unique_handle{wsl::windows::common::wslutil::DuplicateHandle(ClientProcess)}, callbackId};
    const auto [_, inserted] = Entry.Leases.emplace(LeaseId, std::move(lease));
    THROW_HR_IF(E_UNEXPECTED, !inserted);

    auto rollback = wil::scope_exit([&] {
        Entry.Leases.erase(LeaseId);
        (void)m_lifetimeManager.RemoveCallback(callbackId);
    });
    m_lifetimeManager.RegisterCallback(
        callbackId,
        [this, LeaseId] {
            OnClientTerminated(LeaseId);
            return true;
        },
        ClientProcess);
    rollback.release();
}

void ConsoleStateManager::OnClientTerminated(_In_ const GUID& LeaseId)
{
    std::lock_guard lock(m_lock);
    const auto entry = FindLease(LeaseId);
    if (entry == m_entries.end())
    {
        return;
    }

    switch (entry->second.CurrentState)
    {
    case State::Initializing:
        m_entries.erase(entry);
        m_stateChanged.notify_all();
        break;

    case State::Active:
        entry->second.Leases.erase(LeaseId);
        if (entry->second.Leases.empty())
        {
            m_entries.erase(entry);
            m_stateChanged.notify_all();
        }
        break;

    case State::Restoring:
        if (IsEqualGUID(entry->second.Restorer, LeaseId))
        {
            m_entries.erase(entry);
            m_stateChanged.notify_all();
        }
        break;
    }
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
