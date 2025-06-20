/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Dmesg.h

Abstract:

    This file contains declarations used to collect dmesg output

--*/

#pragma once

#include "relay.hpp"
#include "RingBuffer.h"

class DmesgCollector : public std::enable_shared_from_this<DmesgCollector>
{
public:
    DmesgCollector() = delete;
    ~DmesgCollector();

    std::wstring EarlyConsoleName() const
    {
        return m_earlyConsoleName;
    }

    std::wstring VirtioConsoleName() const
    {
        return m_virtioConsoleName;
    }

    static std::shared_ptr<DmesgCollector> Create(
        GUID VmId, const wil::unique_event& ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName, bool EnableEarlyBootConsole);

private:
    enum InputSource
    {
        DmesgCollectorEarlyConsole,
        DmesgCollectorConsole
    };

    DmesgCollector(GUID VmId, const wil::unique_event& ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName);

    HRESULT Start(bool EnableEarlyBootConsole);
    std::pair<std::wstring, std::thread> StartDmesgThread(InputSource Source);
    void ProcessInput(InputSource Source, const gsl::span<char>& Input);
    void WriteToCom1(const gsl::span<char>& Input);

    wil::srwlock m_lock;
    std::wstring m_com1PipeName;
    std::wstring m_earlyConsoleName;
    std::wstring m_virtioConsoleName;
    wil::unique_event m_exitEvent;
    wil::unique_event m_threadExit;
    std::vector<HANDLE> m_exitEvents;
    wil::unique_hfile m_com1Pipe;
    GUID m_runtimeId{};
    wil::unique_event m_overlappedEvent;
    _Guarded_by_(m_lock) OVERLAPPED m_overlapped {};
    RingBuffer m_dmesgBuffer{LX_RELAY_BUFFER_SIZE};
    RingBuffer m_dmesgEarlyBuffer{LX_RELAY_BUFFER_SIZE};
    bool m_debugConsole;
    bool m_telemetry;
    std::atomic<bool> m_earlyConsoleTransition = false;
    bool m_pipeServer;
    bool m_waitForConnection;
    std::thread m_earlyConsoleWorker;
    std::thread m_virtioWorker;
};
