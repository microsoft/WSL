// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WslcSupport.h"

namespace wsl::audit {

struct PluginServices
{
    std::function<AuditPolicy()> queryPolicy;
    std::function<std::wstring()> payloadFolder;
    std::function<std::shared_ptr<AuditSink>(const AuditPolicy&)> createSink;
};

class PluginRuntime
{
public:
    PluginRuntime(const WSLPluginAPIV1& api, PluginServices services);
    bool SupportsWslc() const noexcept
    {
        return m_wslc.has_value();
    }
    HRESULT VmStarted(const WSLSessionInformation* session) noexcept;
    HRESULT VmStopping(const WSLSessionInformation* session) noexcept;
    HRESULT DistributionStarted(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept;
    HRESULT DistributionStopping(const WSLSessionInformation* session, const WSLDistributionInformation* distribution) noexcept;
    HRESULT WslcVmStarted(const WSLCSessionInformation* session) noexcept;
    HRESULT WslcVmStopping(const WSLCSessionInformation* session) noexcept;
    HRESULT ContainerStarted(const WSLCSessionInformation* session, LPCSTR inspect) noexcept;
    HRESULT ContainerStopping(const WSLCSessionInformation* session, LPCSTR containerId) noexcept;

private:
    struct SessionContext;
    using SessionKey = std::pair<bool, DWORD>;
    void Start(SessionKey key, HostIdentity host);
    void Stop(SessionKey key);
    void StopContext(const std::shared_ptr<SessionContext>& context);
    std::shared_ptr<SessionContext> GetContext(SessionKey key);
    static bool Receive(SessionContext& context);
    static DWORD WINAPI ReceiverThread(void* argument) noexcept;
    HRESULT FailStartup(bool wslc) const noexcept;

    WSLPluginAPI_MountFolder m_mount{};
    WSLPluginAPI_ExecuteBinary m_execute{};
    WSLPluginAPI_PluginError m_pluginError{};
    std::optional<WslcApi> m_wslc;
    PluginServices m_services;
    std::mutex m_lock;
    std::map<SessionKey, std::shared_ptr<SessionContext>> m_sessions;
};

} // namespace wsl::audit
