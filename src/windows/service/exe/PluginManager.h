/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginManager.h

Abstract:

    This file contains the PluginManager class definition.
    Plugins are loaded out-of-process in wslpluginhost.exe via COM
    to isolate the service from plugin crashes.

--*/

#pragma once

#include <wil/resource.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "WslPluginApi.h"
#include "WslPluginHost.h"

namespace wsl::windows::service {

//
// IWslPluginHostCallback implementation — lives in the service process and
// handles API calls coming from the plugin host (MountFolder, ExecuteBinary, etc.)
//
class PluginHostCallbackImpl
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslPluginHostCallback>
{
public:
    STDMETHODIMP MountFolder(_In_ DWORD SessionId, _In_ LPCWSTR WindowsPath, _In_ LPCWSTR LinuxPath, _In_ BOOL ReadOnly, _In_ LPCWSTR Name) override;

    STDMETHODIMP ExecuteBinary(
        _In_ DWORD SessionId, _In_ LPCSTR Path, _In_ DWORD ArgumentCount, _In_reads_opt_(ArgumentCount) LPCSTR* Arguments, _Out_ HANDLE* Socket) override;

    STDMETHODIMP ExecuteBinaryInDistribution(
        _In_ DWORD SessionId,
        _In_ const GUID* DistributionId,
        _In_ LPCSTR Path,
        _In_ DWORD ArgumentCount,
        _In_reads_opt_(ArgumentCount) LPCSTR* Arguments,
        _Out_ HANDLE* Socket) override;

    STDMETHODIMP PluginError(_In_ LPCWSTR UserMessage) override;
};

/// <summary>
/// Manages out-of-process plugin hosts (wslpluginhost.exe) via COM activation.
/// Each plugin DLL is loaded in a separate process to isolate the service from
/// plugin crashes. Communication uses IWslPluginHost (service → host) for lifecycle
/// notifications and IWslPluginHostCallback (host → service) for plugin API calls.
/// A job object ensures all hosts are terminated if the service exits unexpectedly.
/// </summary>
class PluginManager
{
public:
    struct PluginError
    {
        std::wstring plugin;
        HRESULT error;
    };

    PluginManager() = default;
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    void LoadPlugins();
    void OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings);
    void OnVmStopping(const WSLSessionInformation* Session);
    void OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* distro);
    void OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* distro);
    void OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro);
    void OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro);
    void ThrowIfFatalPluginError();

private:
    struct OutOfProcPlugin
    {
        Microsoft::WRL::ComPtr<IWslPluginHost> host;
        std::wstring name;
        std::wstring path;
    };

    void LoadPlugin(OutOfProcPlugin& plugin);
    void EnsureInitialized();
    void EnsureJobObjectCreated();
    static void ThrowIfPluginError(HRESULT Result, LPWSTR ErrorMessage, WSLSessionId session, LPCWSTR Plugin);
    static std::vector<BYTE> SerializeSid(PSID Sid);
    static bool IsHostCrash(HRESULT hr);

    std::once_flag m_initOnce;
    std::vector<OutOfProcPlugin> m_plugins;
    Microsoft::WRL::ComPtr<PluginHostCallbackImpl> m_callback;
    std::optional<PluginError> m_pluginError;

    // Job object that automatically terminates all plugin host processes
    // when wslservice exits or crashes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
    std::once_flag m_jobObjectOnce;
    wil::unique_handle m_jobObject;
};

} // namespace wsl::windows::service