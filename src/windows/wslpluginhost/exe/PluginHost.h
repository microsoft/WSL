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
    PluginHost();
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

    //
    // WSLC plugin hooks. UserToken and SidData are marshaled the same way as
    // the existing WSL hooks. The string DisplayName plus ApplicationPid make
    // up the rest of the WSLCSessionInformation struct.
    //

    STDMETHODIMP OnWslcSessionCreated(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData,
        _Outptr_result_maybenull_ LPWSTR* ErrorMessage) override;

    STDMETHODIMP OnWslcSessionStopping(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData) override;

    STDMETHODIMP OnWslcContainerStarted(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData,
        _In_ LPCSTR InspectJson,
        _Outptr_result_maybenull_ LPWSTR* ErrorMessage) override;

    STDMETHODIMP OnWslcContainerStopping(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData,
        _In_ LPCSTR ContainerId) override;

    STDMETHODIMP OnWslcImageCreated(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData,
        _In_ LPCSTR InspectJson) override;

    STDMETHODIMP OnWslcImageDeleted(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData,
        _In_ LPCSTR ImageId) override;

private:
    // Build a WSLSessionInformation struct from the marshaled parameters.
    // The returned struct and its SID allocation are valid for the lifetime of the wil::unique_handle.
    struct SessionContext
    {
        WSLSessionInformation info{};
        std::vector<BYTE> sidBuffer;
    };

    SessionContext BuildSessionContext(DWORD SessionId, HANDLE UserToken, DWORD SidSize, BYTE* SidData);

    // WSLC counterpart — builds a WSLCSessionInformation. DisplayName is held
    // by the caller (the IDL marshaled buffer lives for the call), so the info
    // struct's LPCWSTR DisplayName points at it directly. UserToken/SidData are
    // optional in the WSLC hooks (NotifySessionStopping is invoked without an
    // application token in dtor paths).
    struct WslcSessionContext
    {
        WSLCSessionInformation info{};
        std::vector<BYTE> sidBuffer;
    };

    WslcSessionContext BuildWslcSessionContext(DWORD SessionId, LPCWSTR DisplayName, DWORD ApplicationPid, HANDLE UserToken, DWORD SidSize, BYTE* SidData);

    // Local stubs for the WSLPluginAPIV1 function pointers.
    // These forward calls to the service via m_callback.
    static HRESULT CALLBACK LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name);
    static HRESULT CALLBACK LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);
    static HRESULT CALLBACK LocalPluginError(LPCWSTR UserMessage);
    static HRESULT CALLBACK LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

    // WSLC API stubs. WSLCProcessHandle is a heap-allocated wrapper around the
    // DWORD process cookie owned by the service-side PluginHostCallbackImpl.
    static HRESULT CALLBACK LocalWslcMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, LPCSTR Mountpoint, BOOL ReadOnly);
    static HRESULT CALLBACK LocalWslcUnmountFolder(WSLCSessionId Session, LPCSTR Mountpoint);
    static HRESULT CALLBACK LocalWslcCreateProcess(
        WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcessHandle* Process, int* Errno);
    static HRESULT CALLBACK LocalWslcProcessGetFd(WSLCProcessHandle Process, WSLCProcessFd Fd, HANDLE* Handle);
    static HRESULT CALLBACK LocalWslcProcessGetExitEvent(WSLCProcessHandle Process, HANDLE* ExitEvent);
    static HRESULT CALLBACK LocalWslcProcessGetExitCode(WSLCProcessHandle Process, int* ExitCode);
    static void CALLBACK LocalWslcReleaseProcess(WSLCProcessHandle Process);

    wil::unique_hmodule m_module;
    WSLPluginHooksV1 m_hooks{};
    Microsoft::WRL::ComPtr<IWslPluginHostCallback> m_callback;

    // Serializes hook dispatch so m_pluginErrorMessage and g_hookThreadId
    // are not raced when multiple sessions call hooks concurrently (MTA).
    std::mutex m_hookLock;

    // Error message captured by LocalPluginError during hook execution
    std::optional<std::wstring> m_pluginErrorMessage;
};

// Process-wide pointer to the single PluginHost instance. Safe because
// REGCLS_SINGLEUSE guarantees one PluginHost per wslpluginhost.exe process.
// This allows plugin DLLs to call API functions from any thread, not just
// the thread dispatching the current hook. Atomic so concurrent stub calls
// from plugin worker threads observe a coherent value during ctor/dtor.
extern std::atomic<PluginHost*> g_pluginHost;

// Number of PluginHost instances ever constructed in this process. Used by
// main() to detect orphan hosts that COM-activated wslpluginhost.exe but
// never followed through with a successful CreateInstance.
extern std::atomic<unsigned int> g_activationCount;

} // namespace wsl::windows::pluginhost
