#include "precomp.h"
#include "WSLAContainerProcess.h"
#include "WSLAContainer.h"

using wsl::windows::service::wsla::WSLAContainerProcess;

WSLAContainerProcess::WSLAContainerProcess(
    const std::string& Id,
    bool Tty,
    DockerHTTPClient& client,
    const std::optional<std::string>& ParentContainerId,
    ContainerEventTracker& tracker,
    WSLAContainerImpl& Container) :
    m_id(Id), m_dockerClient(client), m_tty(Tty), m_exec(ParentContainerId.has_value()), m_container(&Container)
{
    // Register for exit events.
    if (m_exec)
    {
        m_trackingReference = tracker.RegisterExecStateUpdates(
            ParentContainerId.value(), Id, std::bind(&WSLAContainerProcess::OnExecEvent, this, std::placeholders::_1, std::placeholders::_2));
    }
}

WSLAContainerProcess::~WSLAContainerProcess()
{
    // TODO: consider moving this to a different class.
    if (m_relayThread.has_value() && m_relayThread->joinable())
    {
        m_exitRelayEvent.SetEvent();

        m_relayThread->join();
    }

    std::lock_guard lock{m_mutex};
    if (m_container != nullptr)
    {
        m_container->OnProcessReleased(this);
    }
}

void WSLAContainerProcess::OnContainerReleased()
{
    std::lock_guard lock{m_mutex};

    WI_ASSERT(m_container != nullptr);
    m_container = nullptr;

    // Signal the exit event to prevent callers being blocked on it.

    OnExited(137); // 128 + 9 (SIGKILL)
}

void WSLAContainerProcess::AssignIoStream(wil::unique_handle&& IoStream)
{
    m_ioStream = std::move(IoStream);
}

void WSLAContainerProcess::OnExecEvent(ContainerEvent Event, std::optional<int> ExitCode)
{
    if (Event == ContainerEvent::ExecDied)
    {
        THROW_HR_IF(E_UNEXPECTED, !ExitCode.has_value());
        OnExited(ExitCode.value());
    }
}

HRESULT WSLAContainerProcess::Signal(_In_ int Signal)
try
{
    THROW_WIN32_IF(ERROR_NOT_SUPPORTED, m_exec);

    try
    {
        m_dockerClient.SignalContainer(m_id, Signal);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to signal container process %hs with signal %d: %hs", m_id.c_str(), Signal, e.what());
    }

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainerProcess::GetExitEvent(_Out_ ULONG* Event)
try
{
    *Event = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(m_exitEvent.get()));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainerProcess::GetStdHandle(_In_ ULONG Index, _Out_ ULONG* Handle)
try
{
    std::lock_guard lock{m_mutex};

    auto& handle = GetStdHandle(Index);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !handle.is_valid());

    *Handle = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(handle.get()));
    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(Index, "fd"),
        TraceLoggingValue(handle.get(), "handle"),
        TraceLoggingValue(*Handle, "remoteHandle"));

    handle.reset();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainerProcess::GetPid(_Out_ int* Pid)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainerProcess::GetState(_Out_ WSLA_PROCESS_STATE* CurrentState, _Out_ int* Code)
try
{
    std::tie(*CurrentState, *Code) = State();

    return S_OK;
}
CATCH_RETURN();

std::pair<WSLA_PROCESS_STATE, int> WSLAContainerProcess::State() const
{
    if (m_exitEvent.is_signaled())
    {
        // TODO: Handle signals.
        return {WSLA_PROCESS_STATE::WslaProcessStateExited, m_exitedCode};
    }
    else
    {
        return {WSLA_PROCESS_STATE::WslaProcessStateRunning, -1};
    }
}

HRESULT WSLAContainerProcess::ResizeTty(_In_ ULONG Rows, _In_ ULONG Columns)
try
{
    std::lock_guard lock{m_mutex};
    RETURN_HR_IF(E_INVALIDARG, !m_tty);

    try
    {
        if (m_exec)
        {
            m_dockerClient.ResizeExecTty(m_id, Rows, Columns);
        }
        else
        {
            m_dockerClient.ResizeContainerTty(m_id, Rows, Columns);
        }
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to resize tty for process %hs: %hs", m_id.c_str(), e.what());
    }

    return S_OK;
}
CATCH_RETURN();

wil::unique_handle& WSLAContainerProcess::GetStdHandle(int Index)
{
    std::lock_guard lock{m_mutex};

    if (m_tty)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, Index != 0, "Invalid fd index for tty process: %i", Index);

        return m_ioStream;
    }
    else
    {
        if (!m_relayedHandles.has_value())
        {
            StartIORelay();
        }

        THROW_HR_IF_MSG(E_INVALIDARG, Index > m_relayedHandles->size(), "Invalid fd index for non-tty process: %i", Index);

        return m_relayedHandles->at(Index);
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid fd index: %i", Index);
}

void WSLAContainerProcess::RunIORelay(HANDLE exitEvent, wil::unique_hfile&& stdinPipe, wil::unique_hfile&& stdoutPipe, wil::unique_hfile&& stderrPipe)
try
{
    common::relay::MultiHandleWait io;

    // This is required for docker to know when stdin is closed.
    auto onInputComplete = [&]() {
        LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(m_ioStream.get()), SD_SEND) == SOCKET_ERROR);
    };

    io.AddHandle(std::make_unique<common::relay::RelayHandle>(
        common::relay::HandleWrapper{std::move(stdinPipe), std::move(onInputComplete)}, m_ioStream.get()));

    io.AddHandle(std::make_unique<common::relay::EventHandle>(exitEvent, [&]() { io.Cancel(); }));
    io.AddHandle(std::make_unique<common::relay::DockerIORelayHandle>(m_ioStream.get(), std::move(stdoutPipe), std::move(stderrPipe)));

    io.Run({});

    // IO relay is done, check the process exit status.
}
CATCH_LOG();

void WSLAContainerProcess::StartIORelay()
{
    std::lock_guard lock{m_mutex};

    WI_ASSERT(!m_relayThread.has_value());
    WI_ASSERT(!m_exitRelayEvent);
    WI_ASSERT(!m_relayedHandles.has_value());

    m_exitRelayEvent.create(wil::EventOptions::ManualReset);
    m_relayedHandles.emplace();

    auto createPipe = []() {
        std::pair<wil::unique_handle, wil::unique_handle> pipe;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&pipe.first, &pipe.second, nullptr, 0));
        return pipe;
    };

    auto stdinPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto stdoutPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto stderrPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

    m_relayedHandles->emplace_back(stdinPipe.second.release());
    m_relayedHandles->emplace_back(stdoutPipe.first.release());
    m_relayedHandles->emplace_back(stderrPipe.first.release());

    m_relayThread.emplace([this,
                           event = m_exitRelayEvent.get(),
                           stdinPipe = std::move(stdinPipe.first),
                           stdoutPipe = std::move(stdoutPipe.second),
                           stderrPipe = std::move(stderrPipe.second)]() mutable {
        RunIORelay(event, std::move(stdinPipe), std::move(stdoutPipe), std::move(stderrPipe));
    });
}

void WSLAContainerProcess::OnExited(int Code)
{
    std::lock_guard lock{m_mutex};

    m_trackingReference.Reset();

    // N.B. OnExited() can be called when the container terminates. If we have already received an exit code for the process, ignore.
    if (!m_exitEvent.is_signaled())
    {
        m_exitedCode = Code;
        m_exitEvent.SetEvent();
    }
}