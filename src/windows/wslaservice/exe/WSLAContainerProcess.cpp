#include "precomp.h"
#include "WSLAContainerProcess.h"

using wsl::windows::service::wsla::WSLAContainerProcess;

WSLAContainerProcess::WSLAContainerProcess(std::string&& Id, wil::unique_handle&& IoStream, bool Tty, DockerHTTPClient& client) :
    m_id(std::move(Id)), m_ioStream(std::move(IoStream)), m_dockerClient(client), m_tty(Tty)
{
}
WSLAContainerProcess::~WSLAContainerProcess()
{

    // TODO: consider moving this to a different class.
    if (m_relayThread.has_value())
    {
        m_exitRelayEvent.SetEvent();

        m_relayThread->join();
    }
}

HRESULT WSLAContainerProcess::Signal(_In_ int Signal)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainerProcess::GetExitEvent(_Out_ ULONG* Event)
{
    *Event = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(m_exitEvent.get()));

    return S_OK;
}

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

    return E_NOTIMPL;
}
CATCH_RETURN();

HRESULT WSLAContainerProcess::GetPid(_Out_ int* Pid)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainerProcess::GetState(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code)
{
    if (m_exitEvent.is_signaled())
    {
        // TODO: Handle signals.
        *State = WSLA_PROCESS_STATE::WslaProcessStateExited;
        *Code = m_exitedCode;
    }
    else
    {
        *State = WSLA_PROCESS_STATE::WslaProcessStateRunning;
    }

    return S_OK;
}

HRESULT WSLAContainerProcess::ResizeTty(_In_ ULONG Rows, _In_ ULONG Columns)
try
{
    std::lock_guard lock{m_mutex};
    RETURN_HR_IF(E_INVALIDARG, !m_tty);

    m_dockerClient.ResizeContainerTty(m_id, Rows, Columns);

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
        THROW_HR_IF_MSG(E_INVALIDARG, Index > m_relayedHandles->size(), "Invalid fd index for non-tty process: %i", Index);

        return m_relayedHandles->at(Index);
    }

    if (Index == 0 && m_tty)
    {
        return m_ioStream;
    }

    return m_ioStream; // TODO: fix
}

void WSLAContainerProcess::RunIORelay(HANDLE exitEvent, wil::unique_handle&& stdinPipe, wil::unique_handle&& stdoutPipe, wil::unique_handle&& stderrPipe)
try
{
    common::relay::MultiHandleWait io;

    io.AddHandle(std::make_unique<common::relay::EventHandle>(exitEvent, [&]() { io.Cancel(); }));
    io.AddHandle(std::make_unique<common::relay::RelayHandle>(std::move(stdinPipe), m_ioStream.get()));
    io.AddHandle(std::make_unique<common::relay::DockerIORelayHandle>(m_ioStream.get(), std::move(stdoutPipe), std::move(stderrPipe)));

    io.Run({});
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

    auto stdinPipe = createPipe();
    auto stdoutPipe = createPipe();
    auto stderrPipe = createPipe();

    m_relayedHandles->emplace_back(std::move(stdinPipe.second));
    m_relayedHandles->emplace_back(std::move(stdoutPipe.first));
    m_relayedHandles->emplace_back(std::move(stdoutPipe.first));

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
    WI_ASSERT(!m_exitEvent.is_signaled());

    m_exitedCode = Code;
    m_exitEvent.SetEvent();
}