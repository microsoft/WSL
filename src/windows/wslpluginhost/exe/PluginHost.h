/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginHost.h

Abstract:

    This file contains the COM class that implements IWslPluginHost.
    It loads a plugin DLL and dispatches lifecycle notifications to it,
    forwarding plugin API callbacks to the service via IWslPluginHostCallback.

--*/

#pragma once

#include "WslPluginApi.h"
#include "WslPluginHost.h"

namespace wsl::windows::pluginhost {

class PluginHost : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslPluginHost>
{
public:
    PluginHost() = default;
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // IWslPluginHost
    STDMETHODIMP Initialize(_In_ IWslPluginHostCallback* Callback, _In_ LPCWSTR PluginPath, _In_ LPCWSTR PluginName) override;
    STDMETHODIMP GetProcessHandle(_Out_ HANDLE* ProcessHandle) override;

    STDMETHODIMP OnVMStarted(
        _In_ DWORD SessionId,
        _In_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_(SidSize) BYTE* SidData,
        _In_ DWORD CustomConfigurationFlags,
        _Outptr_result_maybenull_ LPWSTR* ErrorMessage) override;

    STDMETHODIMP OnVMStopping(_In_ DWORD SessionId, _In_ HANDLE UserToken, _In_ DWORD SidSize, _In_reads_(SidSize) BYTE* SidData) override;

    STDMETHODIMP OnDistributionStarted(
        _In_ DWORD SessionId,
        _In_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_(SidSize) BYTE* SidData,
        _In_ const GUID* DistributionId,
        _In_ LPCWSTR DistributionName,
        _In_ ULONGLONG PidNamespace,
        _In_opt_ LPCWSTR PackageFamilyName,
        _In_ DWORD InitPid,
        _In_opt_ LPCWSTR Flavor,
        _In_opt_ LPCWSTR Version,
        _Outptr_result_maybenull_ LPWSTR* ErrorMessage) override;

    STDMETHODIMP OnDistributionStopping(
        _In_ DWORD SessionId,
        _In_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_(SidSize) BYTE* SidData,
        _In_ const GUID* DistributionId,
        _In_ LPCWSTR DistributionName,
        _In_ ULONGLONG PidNamespace,
        _In_opt_ LPCWSTR PackageFamilyName,
        _In_ DWORD InitPid,
        _In_opt_ LPCWSTR Flavor,
        _In_opt_ LPCWSTR Version) override;

    STDMETHODIMP OnDistributionRegistered(
        _In_ DWORD SessionId,
        _In_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_(SidSize) BYTE* SidData,
        _In_ const GUID* DistributionId,
        _In_ LPCWSTR DistributionName,
        _In_opt_ LPCWSTR PackageFamilyName,
        _In_opt_ LPCWSTR Flavor,
        _In_opt_ LPCWSTR Version) override;

    STDMETHODIMP OnDistributionUnregistered(
        _In_ DWORD SessionId,
        _In_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_(SidSize) BYTE* SidData,
        _In_ const GUID* DistributionId,
        _In_ LPCWSTR DistributionName,
        _In_opt_ LPCWSTR PackageFamilyName,
        _In_opt_ LPCWSTR Flavor,
        _In_opt_ LPCWSTR Version) override;

private:
    // Build a WSLSessionInformation struct from the marshaled parameters.
    // The returned struct and its SID allocation are valid for the lifetime of the wil::unique_handle.
    struct SessionContext
    {
        WSLSessionInformation info{};
        wil::unique_handle tokenHandle;
        std::vector<BYTE> sidBuffer;
    };

    SessionContext BuildSessionContext(DWORD SessionId, HANDLE UserToken, DWORD SidSize, BYTE* SidData);

    // Local stubs for the WSLPluginAPIV1 function pointers.
    // These forward calls to the service via m_callback.
    static HRESULT CALLBACK LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name);
    static HRESULT CALLBACK LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);
    static HRESULT CALLBACK LocalPluginError(LPCWSTR UserMessage);
    static HRESULT CALLBACK LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

    wil::unique_hmodule m_module;
    std::wstring m_pluginName;
    WSLPluginHooksV1 m_hooks{};
    Microsoft::WRL::ComPtr<IWslPluginHostCallback> m_callback;

    // Error message captured by LocalPluginError during hook execution
    std::optional<std::wstring> m_pluginErrorMessage;
};

// Process-wide pointer to the single PluginHost instance. Safe because
// REGCLS_SINGLEUSE guarantees one PluginHost per wslpluginhost.exe process.
// This allows plugin DLLs to call API functions from any thread, not just
// the thread dispatching the current hook.
extern PluginHost* g_pluginHost;

} // namespace wsl::windows::pluginhost
