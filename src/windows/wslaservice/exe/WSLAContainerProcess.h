/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainerProcess.h

Abstract:

    Contains the definition for WSLAContainerProcess.

--*/

#pragma once

#include "DockerHTTPClient.h"
#include "wslaservice.h"
#include "ContainerEventTracker.h"
#include "WSLAProcessIO.h"

namespace wsl::windows::service::wsla {

class WSLAContainerImpl;

class DECLSPEC_UUID("3A5DB29D-6D1D-4619-B89D-578EB34C8E52") WSLAContainerProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAProcess, IFastRundown>
{
public:
    WSLAContainerProcess(
        const std::string& Id,
        bool Tty,
        DockerHTTPClient& Client,
        const std::optional<std::string>& ParentContainerId,
        ContainerEventTracker& Tracker,
        WSLAContainerImpl& Container);
    ~WSLAContainerProcess();

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ ULONG* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ ULONG Index, _Out_ ULONG* Handle) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code) override;
    IFACEMETHOD(ResizeTty)(_In_ ULONG Rows, _In_ ULONG Columns) override;

    std::pair<WSLA_PROCESS_STATE, int> State() const;

    void OnExited(int Code);
    void AssignIoStream(wil::unique_handle&& IoStream); // TODO: rework.
    void OnContainerReleased();

private:
    wil::unique_handle& GetStdHandle(int Index);
    void StartIORelay();
    void RunIORelay(HANDLE exitEvent, wil::unique_hfile&& stdinPipe, wil::unique_hfile&& stdoutPipe, wil::unique_hfile&& stderrPipe);
    void OnExecEvent(ContainerEvent Event, std::optional<int> ExitCode);

    wil::unique_handle m_ioStream;
    DockerHTTPClient& m_dockerClient;
    bool m_tty = false;
    bool m_exec = false;
    ContainerEventTracker::ContainerTrackingReference m_trackingReference;
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    int m_exitedCode = -1;
    std::string m_id;
    std::optional<std::thread> m_relayThread;
    wil::unique_event m_exitRelayEvent;
    std::optional<std::vector<wil::unique_handle>> m_relayedHandles;
    WSLAContainerImpl* m_container{};
    std::recursive_mutex m_mutex;
};

} // namespace wsl::windows::service::wsla