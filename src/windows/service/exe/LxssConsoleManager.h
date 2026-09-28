/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleManager.h

Abstract:

    This file contains function declarations around console management.

--*/

#pragma once
#include "precomp.h"
#include "LxssCreateProcess.h"
#include "Lifetime.h"

class ConsoleManager : public std::enable_shared_from_this<ConsoleManager>
{
public:
    static std::shared_ptr<ConsoleManager> CreateConsoleManager(_In_ const std::shared_ptr<LxssPort>& Port);

    ~ConsoleManager();

    std::shared_ptr<LxssPort> GetSessionLeader(_In_ const CreateLxProcessConsoleData& ConsoleData, _In_ bool Elevated, _Out_opt_ bool* Created = nullptr);

private:
    ConsoleManager() = delete;
    ConsoleManager(_In_ const std::shared_ptr<LxssPort>& Port);

    std::shared_ptr<LxssPort> _RegisterProcess(_In_ const CreateLxProcessConsoleData& ConsoleData, _In_ bool Elevated, _Out_ ULONG* ConsoleId);

    void _SetPort(_In_ ULONG ConsoleId, _In_ bool Elevated, _In_ std::shared_ptr<LxssPort>& Port);

    void _UnregisterProcess(_In_ const wil::unique_handle& ConsoleHandle, _In_ bool Elevated);

    struct SessionLeaderKey
    {
        ULONG ConsoleId;
        bool Elevated;
        bool operator<(const SessionLeaderKey& other) const
        {
            return std::tie(ConsoleId, Elevated) < std::tie(other.ConsoleId, other.Elevated);
        }

        bool operator==(const SessionLeaderKey& other) const
        {
            return ConsoleId == other.ConsoleId && Elevated == other.Elevated;
        }
    };

    struct SessionLeaderMapping
    {
        wil::unique_handle console;
        wil::unique_handle firstClient;
        std::shared_ptr<LxssPort> port;
        ULONG64 clientCallbackId;
    };

    static bool s_OnProcessTerminated(_In_ ConsoleManager* Self, _In_ ULONG ConsoleId, _In_ bool Elevated);

    void _OnProcessDisconnect(_In_ ULONG ConsoleId, _In_ bool Elevated);

    static void _GetConsoleInfo(_In_ const wil::unique_handle& ConsoleHandle, _Out_ ULONG& ConsoleId, _Out_ wil::unique_handle& ConhostHandle);

    static ULONG s_GetConhostServerId(_In_ HANDLE ConsoleHandle);

    std::mutex m_mappingListLock;

    _Guarded_by_(m_mappingListLock) std::map<SessionLeaderKey, SessionLeaderMapping> m_mappings;
    std::shared_ptr<LxssPort> m_initPort;
    LifetimeManager m_lifetimeManager;
};
