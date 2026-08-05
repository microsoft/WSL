/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleManager.cpp

Abstract:

    This file contains function definitions around console management.

--*/

#include "precomp.h"
#include "LxssConsoleManager.h"

using namespace std::placeholders;

ConsoleManager::ConsoleManager(_In_ const std::shared_ptr<LxssPort>& Port) : m_initPort(Port)
{
}

ConsoleManager::~ConsoleManager()
{
    //
    // N.B. Leave the callback cleanup to the lifetime manager destructor.
    //      Because a shared pointer to the ConsoleManager is passed as a
    //      parameter when adding process callback to lifetime manager, the only
    //      way this destructor could be reached is if the callbacks are no
    //      longer valid.
    //
}

std::shared_ptr<ConsoleManager> ConsoleManager::CreateConsoleManager(_In_ const std::shared_ptr<LxssPort>& Port)
{
    std::shared_ptr<ConsoleManager> newConsoleManager(new ConsoleManager(Port));
    return newConsoleManager;
}

std::shared_ptr<LxssPort> ConsoleManager::GetSessionLeader(_In_ const CreateLxProcessConsoleData& ConsoleData, _In_ bool Elevated, _Out_opt_ bool* Created)
{
    auto lock = m_initPort->Lock();
    ULONG ConsoleId = ULONG_MAX;
    auto LocalPort = _RegisterProcess(ConsoleData, Elevated, &ConsoleId);
    auto ConsoleManagerEraser =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _UnregisterProcess(ConsoleData.ConsoleHandle, Elevated); });

    //
    // If the session leader doesn't exist yet for a given console, create one.
    //

    bool createdLocal = false;
    if (!LocalPort)
    {
        LocalPort = m_initPort->CreateSessionLeader(ConsoleData.ClientProcess.get());
        _SetPort(ConsoleId, Elevated, LocalPort);
        createdLocal = true;
    }

    ConsoleManagerEraser.release();
    if (ARGUMENT_PRESENT(Created))
    {
        *Created = createdLocal;
    }

    return LocalPort;
}

std::shared_ptr<LxssPort> ConsoleManager::_RegisterProcess(_In_ const CreateLxProcessConsoleData& ConsoleData, _In_ bool Elevated, _Out_ ULONG* ConsoleId)
{
    ULONG ConsoleIdLocal;
    wil::unique_handle ConhostHandle;
    std::shared_ptr<LxssPort> Port;
    ULONG64 ClientCallbackId;

    _GetConsoleInfo(ConsoleData.ConsoleHandle, ConsoleIdLocal, ConhostHandle);
    {
        std::lock_guard<std::mutex> lock(m_mappingListLock);
        SessionLeaderKey key{ConsoleIdLocal, Elevated};
        const auto mapping = m_mappings.find({ConsoleIdLocal, Elevated});
        if (mapping == m_mappings.end())
        {
            SessionLeaderMapping newMapping;
            newMapping.console = std::move(ConhostHandle);
            newMapping.clientCallbackId = m_lifetimeManager.GetRegistrationId();
            THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
                GetCurrentProcess(), ConsoleData.ClientProcess.get(), GetCurrentProcess(), &newMapping.firstClient, 0, FALSE, DUPLICATE_SAME_ACCESS));

            newMapping.port = nullptr;
            ClientCallbackId = newMapping.clientCallbackId;
            m_mappings.emplace(std::make_pair(key, std::move(newMapping)));
        }
        else
        {
            Port = mapping->second.port;
            ClientCallbackId = mapping->second.clientCallbackId;
        }

        m_lifetimeManager.RegisterCallback(
            ClientCallbackId, std::bind(s_OnProcessTerminated, this, ConsoleIdLocal, Elevated), ConsoleData.ClientProcess.get());
    }

    *ConsoleId = ConsoleIdLocal;
    return Port;
}

void ConsoleManager::_SetPort(_In_ ULONG ConsoleId, _In_ bool Elevated, _In_ std::shared_ptr<LxssPort>& Port)
{
    std::lock_guard<std::mutex> lock(m_mappingListLock);
    const auto mapping = m_mappings.find(SessionLeaderKey{ConsoleId, Elevated});
    if (mapping != m_mappings.end())
    {
        mapping->second.port = Port;
    }
}

void ConsoleManager::_UnregisterProcess(_In_ const wil::unique_handle& ConsoleHandle, _In_ bool Elevated)
{
    ULONG ConsoleId;
    wil::unique_handle ConhostHandle;
    _GetConsoleInfo(ConsoleHandle, ConsoleId, ConhostHandle);
    std::lock_guard<std::mutex> lock(m_mappingListLock);
    const auto mapping = m_mappings.find(SessionLeaderKey{ConsoleId, Elevated});
    if (mapping != m_mappings.end())
    {
        WI_VERIFY(m_lifetimeManager.RemoveCallback(mapping->second.clientCallbackId));
        m_mappings.erase(mapping);
    }
}

void ConsoleManager::_OnProcessDisconnect(_In_ ULONG ConsoleId, _In_ bool Elevated)
{
    wil::unique_handle firstClient;
    std::shared_ptr<LxssPort> port;
    {
        std::lock_guard<std::mutex> lock(m_mappingListLock);
        const auto mapping = m_mappings.find(SessionLeaderKey{ConsoleId, Elevated});
        if (mapping != m_mappings.end())
        {
            if (!m_lifetimeManager.IsAnyProcessRegistered(mapping->second.clientCallbackId))
            {
                firstClient = std::move(mapping->second.firstClient);
                port = mapping->second.port;
                m_mappings.erase(mapping);
            }
        }
    }

    if (firstClient && port)
    {
        port->DisconnectConsole(firstClient.get());
    }
}

void ConsoleManager::_GetConsoleInfo(_In_ const wil::unique_handle& ConsoleHandle, _Out_ ULONG& ConsoleId, _Out_ wil::unique_handle& ConhostHandle)
{
    // A missing console uses a shared identifier for the legacy session-leader path.
    if (!ConsoleHandle)
    {
        ConsoleId = 0;
        return;
    }

    ConsoleStateManager::GetConsoleInfo(ConsoleHandle.get(), ConsoleId, ConhostHandle);
}

bool ConsoleManager::s_OnProcessTerminated(_In_ ConsoleManager* Self, _In_ ULONG ConsoleId, _In_ bool Elevated)
{
    Self->_OnProcessDisconnect(ConsoleId, Elevated);
    return true;
}
