#include "precomp.h"
#include "WSLAContainerProcess.h"

using wsl::windows::service::wsla::WSLAContainerProcess;

WSLAContainerProcess::WSLAContainerProcess(std::string&& Id, wil::unique_handle&& IoStream, bool Tty, DockerHTTPClient& client) :
    m_id(std::move(Id)), m_ioStream(std::move(IoStream)), m_dockerClient(client), m_tty(Tty)
{
}

HRESULT WSLAContainerProcess::Signal(_In_ int Signal)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainerProcess::GetExitEvent(_Out_ ULONG* Event)
{
    return E_NOTIMPL;
}

HRESULT WSLAContainerProcess::GetStdHandle(_In_ ULONG Index, _Out_ ULONG* Handle)
try
{
    std::lock_guard lock{m_mutex};

    auto& socket = GetStdHandle(Index);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !socket.is_valid());

    *Handle = HandleToUlong(common::wslutil::DuplicateHandleToCallingProcess(socket.get()));
    WSL_LOG(
        "GetStdHandle",
        TraceLoggingValue(Index, "fd"),
        TraceLoggingValue(socket.get(), "handle"),
        TraceLoggingValue(*Handle, "remoteHandle"));

    socket.reset();
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
    return E_NOTIMPL;
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

    if (Index == 0 && m_tty)
    {
        return m_ioStream;
    }

    return m_ioStream; // TODO: fix
}