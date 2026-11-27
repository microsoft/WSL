/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginManager.h

Abstract:

    This file contains the PluginManager class definition.

--*/

#pragma once

#include <wil/resource.h>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "WslPluginApi.h"

namespace wsl::windows::service {
class PluginManager
{
public:
    struct PluginError
    {
        std::wstring plugin;
        HRESULT error;
    };

    struct PluginSession
    {
        WSLSessionId sessionId;
        uint64_t pluginId;
    };

    PluginManager() = default;

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    void LoadPlugins();
    void OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings);
    void OnVmStopping(const WSLSessionInformation* Session) const;
    void OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* distro);
    void OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* distro) const;
    void OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro) const;
    void OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro) const;

    // WSLC notifications. Returning failure from OnSessionCreated/OnContainerStarted causes the
    // corresponding operation to be aborted. Other notifications log errors and continue.
    void OnWslcSessionCreated(const WSLCSessionInformation* Session);
    void OnWslcSessionStopping(const WSLCSessionInformation* Session) const;
    HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson) const;
    void OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId) const;
    void OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson) const;
    void OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId) const;
    void OnWslcVmStarted(const WSLCSessionInformation* Session) const;
    void OnWslcVmStopping(const WSLCSessionInformation* Session) const;

    static bool IsInWslcNotification() noexcept;

    void ThrowIfFatalPluginError() const;

    std::optional<PluginSession> FindPluginSession(WSLSessionId Session) const;
    void RemoveSession(WSLSessionId Session) noexcept;

private:
    void LoadPlugin(LPCWSTR Name, LPCWSTR Path);
    static void ThrowIfPluginError(HRESULT Result, LPCWSTR Plugin);
    WSLSessionInformation GetPluginSessionInformation(const WSLSessionInformation& Session, uint64_t PluginId) const;

    struct LoadedPlugin
    {
        wil::unique_hmodule module;
        std::wstring name;
        uint64_t id;
        WSLPluginHooksV1 hooks{};
    };

    std::vector<LoadedPlugin> m_plugins;
    std::optional<PluginError> m_pluginError;
    uint64_t m_nextPluginId = 1;
    mutable wil::srwlock m_sessionLock;
    _Guarded_by_(m_sessionLock) mutable std::map<WSLSessionId, PluginSession> m_sessions;
    _Guarded_by_(m_sessionLock) mutable uint64_t m_nextSessionId = 1;
};

} // namespace wsl::windows::service
