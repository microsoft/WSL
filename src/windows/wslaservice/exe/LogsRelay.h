#pragma once

namespace wsl::windows::service::wsla {

class LogsRelay
{

public:
    NON_COPYABLE(LogsRelay);

    LogsRelay() = default;
    ~LogsRelay();

    std::pair<wil::unique_hfile, wil::unique_hfile> Add(wil::unique_socket&& socket);

private:
    void StartRelayThread();
    void StopRelayThread();
    void Run();
    void AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle);

    std::thread m_thread;
    windows::common::relay::MultiHandleWait m_io;
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
};

} // namespace wsl::windows::service::wsla