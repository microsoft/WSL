// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WindowsBridge.h"
#include "WslPluginApi.h"

namespace wsl::audit {

struct WslcApi
{
    WSLCPluginAPI_MountFolder mount{};
    WSLCPluginAPI_UnmountFolder unmount{};
    WSLCPluginAPI_CreateProcess createProcess{};
    WSLCPluginAPI_ProcessGetFd getFd{};
    WSLCPluginAPI_ProcessGetExitEvent getExitEvent{};
    WSLCPluginAPI_ProcessGetExitCode getExitCode{};
    WSLCPluginAPI_ReleaseProcess releaseProcess{};
};

std::optional<WslcApi> GetWslcApi(const WSLPluginAPIV1& api);
ContainerIdentity ReadContainerIdentity(std::string_view inspect);
std::vector<LPCSTR> CollectorArguments(LPCSTR option, LPCSTR value);

class WslcProcess
{
public:
    explicit WslcProcess(WslcApi api);
    void Start(WSLCSessionId session, std::vector<LPCSTR> arguments);
    SOCKET Input() const noexcept
    {
        return m_input.get();
    }
    SOCKET Output() const noexcept
    {
        return m_output.get();
    }
    SOCKET Error() const noexcept
    {
        return m_error.get();
    }
    HANDLE ExitEvent() const noexcept
    {
        return m_exitEvent.get();
    }
    int WaitForExit(DWORD milliseconds) const;
    void Shutdown() const noexcept;

private:
    WslcApi m_api;
    // Sockets and the exit event must be closed before releasing the opaque process handle.
    std::unique_ptr<void, WSLCPluginAPI_ReleaseProcess> m_process;
    wil::unique_handle m_exitEvent;
    UniqueSocket m_input;
    UniqueSocket m_output;
    UniqueSocket m_error;
};

void ResolveContainerNamespaces(const WslcApi& api, WSLCSessionId session, ContainerIdentity& container);

} // namespace wsl::audit
