#pragma once
#include "wslaservice.h"
#include <variant>
#include <vector>
#include <string>

namespace wsl::windows::common {

enum class FDFlags
{
    None = 0,
    Stdin = 1,
    Stdout = 2,
    Stderr = 4,
};

DEFINE_ENUM_FLAG_OPERATORS(FDFlags);

class WSLAProcessWrapper
{
public:
    struct ProcessResult
    {
        int Code;
        bool Signalled;
        std::vector<std::string> Output;
    };

    WSLAProcessWrapper(IWSLASession* Session, std::string&& Executable, std::vector<std::string>&& Arguments, FDFlags Flags = FDFlags::Stdout | FDFlags::Stderr);

    IWSLAProcess& Launch();

    ProcessResult WaitAndCaptureOutput(DWORD TimeoutMs = INFINITE, std::vector<std::unique_ptr<relay::OverlappedIOHandle>>&& ExtraHandles = {});
    ProcessResult LaunchAndCaptureOutput(DWORD TimeoutMs = INFINITE);

private:
    std::function<Microsoft::WRL::ComPtr<IWSLAProcess>(const WSLA_PROCESS_OPTIONS*)> m_launch;
    std::vector<WSLA_PROCESS_FD> m_fds;
    std::string m_executable;
    std::vector<std::string> m_arguments;
    Microsoft::WRL::ComPtr<IWSLAProcess> m_process;
};
} // namespace wsl::windows::common