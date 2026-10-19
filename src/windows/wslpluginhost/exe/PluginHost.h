// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "WslPluginApi.h"
#include "WslPluginHost.h"

namespace wsl::windows::pluginhost {

class PluginHost : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslPluginHost>
{
public:
    PluginHost();
    ~PluginHost();

    NON_COPYABLE(PluginHost);
    NON_MOVABLE(PluginHost);

    // IWslPluginHost
    STDMETHODIMP JoinJob(_In_ HANDLE JobObject) override;
    STDMETHODIMP Initialize(_In_ IWslPluginHostCallback* Callback, _In_ LPCWSTR PluginPath, _In_ LPCWSTR PluginName) override;

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

    STDMETHODIMP OnWslcVmStarted(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData) override;

    STDMETHODIMP OnWslcVmStopping(
        _In_ DWORD SessionId,
        _In_ LPCWSTR DisplayName,
        _In_ DWORD ApplicationPid,
        _In_opt_ HANDLE UserToken,
        _In_ DWORD SidSize,
        _In_reads_opt_(SidSize) BYTE* SidData) override;

private:
    // Build a WSLSessionInformation struct from the marshaled parameters.
    struct SessionContext
    {
        WSLSessionInformation info{};
        std::vector<BYTE> sidBuffer;
    };

    SessionContext BuildSessionContext(DWORD SessionId, HANDLE UserToken, DWORD SidSize, BYTE* SidData);

    // DisplayName and the optional token are borrowed from the marshaled call.
    struct WslcSessionContext
    {
        WSLCSessionInformation info{};
        std::vector<BYTE> sidBuffer;
    };

    WslcSessionContext BuildWslcSessionContext(DWORD SessionId, LPCWSTR DisplayName, DWORD ApplicationPid, HANDLE UserToken, DWORD SidSize, BYTE* SidData);

    // Local stubs for the WSLPluginAPIV1 function pointers.
    // These forward calls through an apartment-local callback proxy.
    static HRESULT CALLBACK LocalMountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name);
    static HRESULT CALLBACK LocalExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);
    static HRESULT CALLBACK LocalPluginError(LPCWSTR UserMessage);
    static HRESULT CALLBACK LocalExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

    // WSLC API stubs. WSLCProcessHandle is a heap-allocated wrapper that owns
    // the IWSLCProcess COM proxy marshaled from wslcsession via the service.
    static HRESULT CALLBACK LocalWslcMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, LPCSTR Mountpoint, BOOL ReadOnly);
    static HRESULT CALLBACK LocalWslcUnmountFolder(WSLCSessionId Session, LPCSTR Mountpoint);
    static HRESULT CALLBACK LocalWslcCreateProcess(
        WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcessHandle* Process, int* Errno);
    static HRESULT CALLBACK LocalWslcProcessGetFd(WSLCProcessHandle Process, WSLCProcessFd Fd, SOCKET* Socket);
    static HRESULT CALLBACK LocalWslcProcessGetExitEvent(WSLCProcessHandle Process, HANDLE* ExitEvent);
    static HRESULT CALLBACK LocalWslcProcessGetExitCode(WSLCProcessHandle Process, int* ExitCode);
    static void CALLBACK LocalWslcReleaseProcess(WSLCProcessHandle Process);

    wil::unique_hmodule m_module;
    WSLPluginHooksV1 m_hooks{};
    static wil::com_ptr<IWslPluginHostCallback> GetCallback();
    wil::com_ptr<IGlobalInterfaceTable> m_git;
    DWORD m_callbackCookie = 0;
    bool m_inJob = false;
    static thread_local std::optional<std::wstring> m_pluginErrorMessage;
};

// REGCLS_SINGLEUSE gives each plugin a separate process.
extern std::atomic<PluginHost*> g_pluginHost;

// Detect activations abandoned before CreateInstance.
extern std::atomic<unsigned int> g_activationCount;

} // namespace wsl::windows::pluginhost
