#pragma once

namespace wsl::windows::service::wsla {

class LogsRelay
{

public:
    NON_COPYABLE(LogsRelay);

    LogsRelay() = default;
    ~LogsRelay();

    void AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle);

private:
    void StartRelayThread();
    void StopRelayThread();
    void Run();

    std::thread m_thread;
    windows::common::relay::MultiHandleWait m_io;
    wil::unique_event m_stopEvent{wil::EventOptions::ManualReset};
};

} // namespace wsl::windows::service::wsla