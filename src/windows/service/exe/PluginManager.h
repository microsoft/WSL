// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wil/resource.h>
#include "WslPluginApi.h"
#include "WslPluginHost.h"
#include "PluginCallPump.h"

namespace wsl::windows::service {

class PluginManager
{
public:
    NON_COPYABLE(PluginManager);
    NON_MOVABLE(PluginManager);

    PluginManager() = default;

    void LoadPlugins();
    void Initialize();
    void Shutdown();
    void ThrowIfFatalPluginError() const;

    void OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings);
    void OnVmStopping(const WSLSessionInformation* Session);
    void OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution);
    void OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution);
    void OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution);
    void OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution);

    void OnWslcSessionCreated(const WSLCSessionInformation* Session);
    void OnWslcSessionStopping(const WSLCSessionInformation* Session);
    HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson);
    void OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId);
    void OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson);
    void OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId);
    void OnWslcVmStarted(const WSLCSessionInformation* Session);
    void OnWslcVmStopping(const WSLCSessionInformation* Session);

    void RegisterWslcSession(ULONG SessionId, IWSLCSessionReference* Reference);
    void UnregisterWslcSession(ULONG SessionId);

private:
    class Callback;

    struct Plugin
    {
        std::wstring name;
        std::wstring path;
        // Created and called only in the MTA, never on incoming NTA/STA threads.
        wil::com_ptr<IWslPluginHost> host;
        std::atomic<HRESULT> failure{S_OK};
    };

    struct PluginError
    {
        std::wstring name;
        HRESULT result;
    };

    using Notification = std::function<HRESULT(IWslPluginHost*, LPWSTR*)>;
    void Notify(const Notification& Work, bool Fatal, std::optional<WSLSessionId> Session = {});
    HRESULT InvokeCallback(WSLSessionId Session, const std::function<HRESULT()>& Work);
    wil::com_ptr<IWSLCSession> ResolveWslcSession(ULONG SessionId);
    void RecordFailure(const Plugin& Plugin, HRESULT Result);
    static std::vector<BYTE> SerializeSid(PSID Sid);
    static bool IsHostDisconnected(HRESULT Result);
    static void ThrowIfPluginError(HRESULT Result, LPCWSTR Message, LPCWSTR Plugin);

    wil::unique_event m_initialized{wil::EventOptions::ManualReset};
    wil::srwlock m_notificationLock;
    _Guarded_by_(m_notificationLock) std::vector<std::unique_ptr<Plugin>> m_plugins;
    wil::unique_handle m_job;
    mutable wil::srwlock m_errorLock;
    _Guarded_by_(m_errorLock) std::optional<PluginError> m_error;
    wil::srwlock m_pumpLock;
    _Guarded_by_(m_pumpLock) std::map<WSLSessionId, std::shared_ptr<PluginCallPump>> m_pumps;
    wil::srwlock m_sessionLock;
    _Guarded_by_(m_sessionLock) wil::com_ptr<IGlobalInterfaceTable> m_git;
    _Guarded_by_(m_sessionLock) std::map<ULONG, DWORD> m_sessions;
};

} // namespace wsl::windows::service
