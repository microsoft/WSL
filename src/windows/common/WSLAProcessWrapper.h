#pragma once
#include "wslaservice.h"
#include <variant>
#include <vector>
#include <string>

namespace wsl::windows::common {
class WSLAProcessWrapper
{
public:
    struct ProcessResult
    {
        int Code;
        bool Signalled;
        std::vector<std::string> Output;
    };

    WSLAProcessWrapper(IWSLASession* Session, std::string&& Executable, std::vector<std::string>&& Arguments);

    IWSLAProcess& Launch();

    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE);
    ProcessResult LaunchAndCaptureOutput(DWORD TimeoutMs = INFINITE);

private:
    std::function<Microsoft::WRL::ComPtr<IWSLAProcess>(const WSLA_PROCESS_OPTIONS*)> m_launch;
    std::vector<WSLA_PROCESS_FD> m_fds;
    std::string m_executable;
    std::vector<std::string> m_arguments;
    Microsoft::WRL::ComPtr<IWSLAProcess> m_process;
};
} // namespace wsl::windows::common