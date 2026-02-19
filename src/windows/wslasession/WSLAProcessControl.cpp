/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessControl.cpp

Abstract:

    Contains the different WSLAProcessControl definitions for process control logic.

--*/

#include "precomp.h"

#include "WSLAProcessControl.h"
#include "WSLAVirtualMachine.h"
#include "WSLAContainer.h"

using wsl::windows::service::wsla::DockerContainerProcessControl;
using wsl::windows::service::wsla::DockerExecProcessControl;
using wsl::windows::service::wsla::VMProcessControl;
using wsl::windows::service::wsla::WSLAProcessControl;

std::pair<WSLA_PROCESS_STATE, int> WSLAProcessControl::GetState() const
{
    if (m_exitEvent.is_signaled())
    {
        WI_ASSERT(m_exitedCode.has_value());
        return {WslaProcessStateExited, m_exitedCode.value()};
    }
    else
    {
        return {WslaProcessStateRunning, -1};
    }
}

HANDLE WSLAProcessControl::GetExitEvent() const
{
    return m_exitEvent.get();
}

DockerContainerProcessControl::DockerContainerProcessControl(WSLAContainerImpl& Container, DockerHTTPClient& DockerClient, ContainerEventTracker& EventTracker) :
    m_container(&Container),
    m_client(DockerClient),
    m_trackingReference(EventTracker.RegisterContainerStateUpdates(
        Container.ID(), std::bind(&DockerContainerProcessControl::OnEvent, this, std::placeholders::_1, std::placeholders::_2)))
{
}

DockerContainerProcessControl::~DockerContainerProcessControl()
{
}

void DockerContainerProcessControl::Signal(int Signal)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_container == nullptr || m_exitEvent.is_signaled());

    m_client.SignalContainer(m_container->ID(), Signal);
}

void DockerContainerProcessControl::ResizeTty(ULONG Rows, ULONG Columns)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_container == nullptr || m_exitEvent.is_signaled());

    m_client.ResizeContainerTty(m_container->ID(), Rows, Columns);
}

void DockerContainerProcessControl::OnEvent(ContainerEvent Event, std::optional<int> ExitCode)
{
    if (Event == ContainerEvent::Stop)
    {
        std::lock_guard lock{m_lock};
        if (!m_exitEvent.is_signaled())
        {
            WSL_LOG("ContainerProcessStop");
            WI_ASSERT(ExitCode.has_value());
            WI_ASSERT(!m_exitedCode.has_value());
            m_exitedCode = ExitCode.value();
            m_exitEvent.SetEvent();
        }
    }
}

int DockerContainerProcessControl::GetPid() const
{
    return 1;
}

void DockerContainerProcessControl::OnContainerReleased()
{
    {
        std::lock_guard lock{m_lock};

        WI_ASSERT(m_container != nullptr);
        m_container = nullptr;
    }

    // N.B. The caller might keep a reference to the process even after the container is released.
    // If that happens, make sure that the state tracking can't outlive the session.
    // This is safe to call without the lock because removing the tracking reference is protected by the event tracker lock.
    m_trackingReference.Reset();

    // Signal the exit event to prevent callers from being blocked on it.
    if (!m_exitEvent.is_signaled())
    {
        m_exitedCode = 128 + WSLASignalSIGKILL;
        m_exitEvent.SetEvent();
    }
}

DockerExecProcessControl::DockerExecProcessControl(
    WSLAContainerImpl& Container, const std::string& Id, DockerHTTPClient& DockerClient, ContainerEventTracker& EventTracker) :
    m_container(&Container),
    m_id(Id),
    m_client(DockerClient),
    m_trackingReference(EventTracker.RegisterExecStateUpdates(
        Container.ID(), Id, std::bind(&DockerExecProcessControl::OnEvent, this, std::placeholders::_1, std::placeholders::_2)))
{
}

DockerExecProcessControl::~DockerExecProcessControl()
{
    std::lock_guard lock{m_lock};
    if (m_container != nullptr)
    {
        m_container->OnProcessReleased(this);
    }
}

int DockerExecProcessControl::GetPid() const
{
    // TODO: implement Inspect() for exec'd processes.
    THROW_HR(E_NOTIMPL);
}

void DockerExecProcessControl::Signal(int Signal)
{
    THROW_WIN32(ERROR_NOT_SUPPORTED);
}

void DockerExecProcessControl::ResizeTty(ULONG Rows, ULONG Columns)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_container == nullptr || m_exitEvent.is_signaled());

    m_client.ResizeExecTty(m_id, Rows, Columns);
}

void DockerExecProcessControl::OnEvent(ContainerEvent Event, std::optional<int> ExitCode)
{
    if (Event == ContainerEvent::ExecDied && !m_exitEvent.is_signaled())
    {
        WI_ASSERT(ExitCode.has_value());
        m_exitedCode = ExitCode.value();
        m_exitEvent.SetEvent();
    }
}

void DockerExecProcessControl::OnContainerReleased()
{
    {
        std::lock_guard lock{m_lock};

        WI_ASSERT(m_container != nullptr);
        m_container = nullptr;
    }

    // N.B. The caller might keep a reference to the process even after the container is released.
    // If that happens, make sure that the state tracking can't outlive the session.
    // This is safe to call without the lock because removing the tracking reference is protected by the event tracker lock.

    m_trackingReference.Reset();

    // Signal the exit event to prevent callers being blocked on it.
    if (!m_exitEvent.is_signaled())
    {
        m_exitedCode = 128 + WSLASignalSIGKILL;
        m_exitEvent.SetEvent();
    }
}

VMProcessControl::VMProcessControl(WSLAVirtualMachine& VirtualMachine, int Pid, wil::unique_socket&& TtyControl) :
    m_pid(Pid), m_ttyControlChannel(std::move(TtyControl), "TtyControl", VirtualMachine.TerminatingEvent()), m_vm(&VirtualMachine)
{
}

VMProcessControl::~VMProcessControl()
{
    std::lock_guard lock{m_lock};

    if (m_vm != nullptr)
    {
        m_vm->OnProcessReleased(m_pid);
    }
}

void VMProcessControl::Signal(int Signal)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_vm == nullptr || m_exitEvent.is_signaled());

    m_vm->Signal(m_pid, Signal);
}

void VMProcessControl::ResizeTty(ULONG Rows, ULONG Columns)
{
    std::lock_guard lock{m_lock};

    THROW_WIN32_IF(ERROR_INVALID_STATE, !m_ttyControlChannel.Connected());
    THROW_HR_IF(E_INVALIDARG, Rows == 0 || Columns == 0 || Rows > USHORT_MAX || Columns > USHORT_MAX);

    WSLA_TERMINAL_CHANGED message{};
    message.Rows = static_cast<unsigned short>(Rows);
    message.Columns = static_cast<unsigned short>(Columns);
    m_ttyControlChannel.SendMessage(message);
}

void VMProcessControl::OnExited(int Code)
{
    std::lock_guard lock{m_lock};

    if (!m_exitEvent.is_signaled())
    {
        m_exitedCode = Code;
        m_ttyControlChannel.Close();
        m_exitEvent.SetEvent();
    }
}

int VMProcessControl::GetPid() const
{
    return m_pid;
}

void VMProcessControl::OnVmTerminated()
{
    std::lock_guard lock{m_lock};
    m_vm = nullptr;

    // Make sure that the process is in a terminated state, so users don't think that it might still be running.
    if (!m_exitEvent.is_signaled())
    {
        m_exitedCode = 128 + WSLASignalSIGKILL;
        m_exitEvent.SetEvent();
    }
}