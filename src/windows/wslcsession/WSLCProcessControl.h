/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCProcessControl.h

Abstract:

    Contains the different WSLCProcessControl definitions for process control logic.

--*/

#pragma once
#include "wslc.h"
#include "DockerHTTPClient.h"
#include "DockerEventTracker.h"

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;
class WSLCContainerImpl;

class WSLCProcessControl
{
public:
    WSLCProcessControl() = default;
    virtual ~WSLCProcessControl() = default;

    virtual void Signal(int Signal) = 0;
    virtual void ResizeTty(ULONG Rows, ULONG Columns) = 0;
    virtual int GetPid() const = 0;
    std::pair<WSLCProcessState, int> GetState() const;
    const wil::unique_event& GetExitEvent() const;

protected:
    wil::unique_event m_exitEvent{wil::EventOptions::ManualReset};
    std::optional<int> m_exitedCode{};
};

class DockerContainerProcessControl : public WSLCProcessControl
{
public:
    DockerContainerProcessControl(WSLCContainerImpl& Container, DockerHTTPClient& DockerClient, DockerEventTracker& EventTracker);
    ~DockerContainerProcessControl();
    void Signal(int Signal) override;
    void ResizeTty(ULONG Rows, ULONG Columns) override;
    int GetPid() const override;
    void OnContainerReleased() noexcept;

private:
    void OnEvent(ContainerEvent Event, std::optional<int> ExitCode, std::uint64_t eventTime);

    std::mutex m_lock;
    DockerHTTPClient& m_client;
    WSLCContainerImpl* m_container{};
    DockerEventTracker::EventTrackingReference m_trackingReference;
};

class DockerExecProcessControl : public WSLCProcessControl
{
public:
    DockerExecProcessControl(WSLCContainerImpl& Container, const std::string& Id, DockerHTTPClient& DockerClient, DockerEventTracker& EventTracker);
    ~DockerExecProcessControl();
    void Signal(int Signal) override;
    void ResizeTty(ULONG Rows, ULONG Columns) override;
    int GetPid() const override;
    void OnContainerReleased() noexcept;

    void SetPid(int Pid);
    void SetExitCode(int ExitCode);

private:
    void OnEvent(ContainerEvent Event, std::optional<int> ExitCode, std::uint64_t eventTime);

    mutable std::mutex m_lock;
    std::string m_id;
    std::optional<int> m_pid{};
    DockerHTTPClient& m_client;
    WSLCContainerImpl* m_container{};
    DockerEventTracker::EventTrackingReference m_trackingReference;
};

class VMProcessControl : public WSLCProcessControl
{
public:
    VMProcessControl(WSLCVirtualMachine& VirtualMachine, int Pid, wil::unique_socket&& TtyControl);
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
    WSLCVirtualMachine* m_vm{};
};

} // namespace wsl::windows::service::wslc