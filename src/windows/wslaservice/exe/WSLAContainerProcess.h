#pragma once

#include "DockerHTTPClient.h"
#include "wslaservice.h"

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("3A5DB29D-6D1D-4619-B89D-578EB34C8E52") WSLAContainerProcess
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAProcess, IFastRundown>
{
public:
    WSLAContainerProcess(std::string&& Id, wil::unique_handle&& IoStream, bool Tty, DockerHTTPClient& client);

    IFACEMETHOD(Signal)(_In_ int Signal) override;
    IFACEMETHOD(GetExitEvent)(_Out_ ULONG* Event) override;
    IFACEMETHOD(GetStdHandle)(_In_ ULONG Index, _Out_ ULONG* Handle) override;
    IFACEMETHOD(GetPid)(_Out_ int* Pid) override;
    IFACEMETHOD(GetState)(_Out_ WSLA_PROCESS_STATE* State, _Out_ int* Code) override;
    IFACEMETHOD(ResizeTty)(_In_ ULONG Rows, _In_ ULONG Columns) override;

private:
    wil::unique_handle& GetStdHandle(int Index);

    wil::unique_handle m_ioStream;
    DockerHTTPClient& m_dockerClient;
    bool m_tty = false;
    std::string m_id;
    std::recursive_mutex m_mutex;
};

} // namespace wsl::windows::service::wsla