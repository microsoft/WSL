/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleStateManager.h

Abstract:

    This file contains declarations for coordinating console state leases.

--*/

#pragma once

#include <condition_variable>
#include <set>

class ConsoleStateManager
{
public:
    void Acquire(_In_ HANDLE ConsoleHandle, _Out_ GUID& LeaseId, _Out_ bool& Initialize, _Out_ LXSS_CONSOLE_STATE& ConfiguredState);
    void Commit(_In_ const GUID& LeaseId, _In_ const LXSS_CONSOLE_STATE& BaselineState, _In_ const LXSS_CONSOLE_STATE& ConfiguredState);
    bool Release(_In_ const GUID& LeaseId, _Out_ LXSS_CONSOLE_STATE& BaselineState, _Out_ LXSS_CONSOLE_STATE& ConfiguredState);
    void Complete(_In_ const GUID& LeaseId);

    static void GetConsoleInfo(_In_ HANDLE ConsoleHandle, _Out_ ULONG& ConsoleId, _Out_ wil::unique_handle& ConhostHandle);

private:
    enum class State
    {
        Initializing,
        Active,
        Restoring
    };

    struct Entry
    {
        wil::unique_handle ConhostHandle;
        State CurrentState{State::Initializing};
        std::set<GUID, wsl::windows::common::helpers::GuidLess> Leases;
        GUID Restorer{};
        LXSS_CONSOLE_STATE BaselineState{};
        LXSS_CONSOLE_STATE ConfiguredState{};
    };

    using EntryIterator = std::map<ULONG, Entry>::iterator;

    _Requires_lock_held_(m_lock)
    EntryIterator FindLease(_In_ const GUID& LeaseId);

    static ULONG GetConhostServerId(_In_ HANDLE ConsoleHandle);

    std::mutex m_lock;
    std::condition_variable m_stateChanged;
    _Guarded_by_(m_lock) std::map<ULONG, Entry> m_entries;
};
