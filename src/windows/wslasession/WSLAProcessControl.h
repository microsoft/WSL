/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessControl.h

Abstract:

    Contains the different WSLAProcessControl definitions for process control logic.

--*/

#pragma once
#include "wslaservice.h"
#include "DockerHTTPClient.h"
#include "ContainerEventTracker.h"

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;
class WSLAContainerImpl;

class WSLAProcessControl
{
public:
    WSLAProcessControl() = default;
    virtual ~WSLAProcessControl() = default;

    virtual void Signal(int Signal) = 0;
    virtual void ResizeTty(ULONG Rows, ULONG Columns) = 0;
    virtual int GetPid() const = 0;
    std::pair<WSLA_PROCESS_STATE, int> GetState() const;
    const wil::unique_event& GetExitEvent() const;

protected:
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    std::optional<int> m_exitedCode{};
};

class DockerContainerProcessControl : public WSLAProcessControl
{
public:
    DockerContainerProcessControl(WSLAContainerImpl& Container, DockerHTTPClient& DockerClient, ContainerEventTracker& EventTracker);
    ~DockerContainerProcessControl();
    void Signal(int Signal) override;
    void ResizeTty(ULONG Rows, ULONG Columns) override;
    int GetPid() const override;
    void OnContainerReleased();

private:
    void OnEvent(ContainerEvent Event, std::optional<int> ExitCode);

    std::mutex m_lock;
    DockerHTTPClient& m_client;
    WSLAContainerImpl* m_container{};
    ContainerEventTracker::ContainerTrackingReference m_trackingReference;
};

class DockerExecProcessControl : public WSLAProcessControl
{
public:
    DockerExecProcessControl(WSLAContainerImpl& Container, const std::string& Id, DockerHTTPClient& DockerClient, ContainerEventTracker& EventTracker);
    ~DockerExecProcessControl();
    void Signal(int Signal) override;
    void ResizeTty(ULONG Rows, ULONG Columns) override;
    int GetPid() const override;
    void OnContainerReleased();

    void SetPid(int Pid);
    void SetExitCode(int ExitCode);

private:
    void OnEvent(ContainerEvent Event, std::optional<int> ExitCode);

    mutable std::mutex m_lock;
    std::string m_id;
    std::optional<int> m_pid{};
    DockerHTTPClient& m_client;
    WSLAContainerImpl* m_container{};
    ContainerEventTracker::ContainerTrackingReference m_trackingReference;
};

class VMProcessControl : public WSLAProcessControl
{
public:
    VMProcessControl(WSLAVirtualMachine& VirtualMachine, int Pid, wil::unique_socket&& TtyControl);
    ~VMProcessControl();

    void Signal(int Signal) override;
    void ResizeTty(ULONG Rows, ULONG Columns) override;
    int GetPid() const override;

    void OnExited(int Code);
    void OnVmTerminated();

private:
    std::mutex m_lock;
    int m_pid{};
    wsl::shared::SocketChannel m_ttyControlChannel;
    WSLAVirtualMachine* m_vm{};
};

} // namespace wsl::windows::service::wsla