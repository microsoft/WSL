/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Dmesg.h

Abstract:

    This file contains declarations used to collect dmesg output

--*/

#pragma once

#include "relay.hpp"
#include "HandleIO.h"
#include "RingBuffer.h"

class DmesgCollector
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
        GUID VmId,
        HANDLE ExitEvent,
        bool EnableTelemetry,
        bool EnableDebugConsole,
        const std::wstring& Com1PipeName,
        bool EnableEarlyBootConsole,
        wil::unique_handle&& OutputHandle);

private:
    enum InputSource
    {
        DmesgCollectorEarlyConsole,
        DmesgCollectorConsole
    };

    DmesgCollector(GUID VmId, HANDLE ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName, wil::unique_handle&& OutputHandle = {});

    void Start(bool EnableEarlyBootConsole);

    void Run();

    static std::pair<std::wstring, wil::unique_hfile> CreateConsolePipe();

    void ProcessInput(InputSource Source, const gsl::span<char>& Input);

    std::wstring m_com1PipeName;
    std::wstring m_earlyConsoleName;
    std::wstring m_virtioConsoleName;
    HANDLE m_vmExitEvent;
    wil::unique_event m_threadExitEvent{wil::EventOptions::ManualReset};
    wil::unique_hfile m_com1Pipe;
    wil::unique_handle m_outputHandle;
    wil::unique_hfile m_earlyConsolePipe;
    wil::unique_hfile m_virtioConsolePipe;
    GUID m_runtimeId{};
    RingBuffer m_dmesgBuffer{LX_RELAY_BUFFER_SIZE};
    RingBuffer m_dmesgEarlyBuffer{LX_RELAY_BUFFER_SIZE};
    bool m_debugConsole;
    bool m_telemetry;
    bool m_earlyConsoleTransition = false;
    bool m_pipeServer = false;

    std::thread m_thread;

    wsl::windows::common::io::WriteHandle* m_outputWrite = nullptr;
    wsl::windows::common::io::WriteNamedPipe* m_com1Write = nullptr;
};
