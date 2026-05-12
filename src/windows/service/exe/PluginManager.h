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
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "WslPluginApi.h"
#include "WslPluginHost.h"
#include "wslc.h"

namespace wsl::windows::service {

class PluginManager;

//
// IWslPluginHostCallback implementation — lives in the service process and
// handles API calls coming from the plugin host (MountFolder, ExecuteBinary,
// WSLC* APIs etc.). One instance is created per plugin host so that the
// per-plugin WSLC process map (cookie -> IWSLCProcess) is isolated: a plugin
// cannot guess another plugin's cookie, and the map drains automatically when
// the plugin host process goes away.
//
class PluginHostCallbackImpl
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslPluginHostCallback>
{
public:
    explicit PluginHostCallbackImpl(PluginManager& owner) : m_owner(owner)
    {
    }
    ~PluginHostCallbackImpl() override = default;

    PluginHostCallbackImpl(const PluginHostCallbackImpl&) = delete;
    PluginHostCallbackImpl& operator=(const PluginHostCallbackImpl&) = delete;

    // WSL plugin API (host -> service)
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

    // WSLC plugin API (host -> service)
    STDMETHODIMP WslcMountFolder(_In_ DWORD SessionId, _In_ LPCWSTR WindowsPath, _In_ LPCSTR Mountpoint, _In_ BOOL ReadOnly) override;

    STDMETHODIMP WslcUnmountFolder(_In_ DWORD SessionId, _In_ LPCSTR Mountpoint) override;

    STDMETHODIMP WslcCreateProcess(
        _In_ DWORD SessionId,
        _In_ LPCSTR Executable,
        _In_ DWORD ArgumentCount,
        _In_reads_opt_(ArgumentCount) LPCSTR* Arguments,
        _In_ DWORD EnvCount,
        _In_reads_opt_(EnvCount) LPCSTR* Environment,
        _Out_ DWORD* ProcessCookie,
        _Out_ int* Errno) override;

    STDMETHODIMP WslcProcessGetFd(_In_ DWORD ProcessCookie, _In_ DWORD Fd, _Out_ HANDLE* Handle) override;

    STDMETHODIMP WslcProcessGetExitEvent(_In_ DWORD ProcessCookie, _Out_ HANDLE* ExitEvent) override;

    STDMETHODIMP WslcProcessGetExitCode(_In_ DWORD ProcessCookie, _Out_ int* ExitCode) override;

    STDMETHODIMP WslcReleaseProcess(_In_ DWORD ProcessCookie) override;

    // Release all outstanding process mappings. Called when the plugin host
    // crashes so the WSLC processes it created aren't stranded until shutdown.
    void DrainProcesses() noexcept;

private:
    // Allocate a new cookie -> process mapping. Loops past 0 and past collisions.
    // Throws on exhaustion.
    DWORD InsertProcessLocked(wil::com_ptr<IWSLCProcess> process);

    // Resolve a cookie to its process under m_processLock; returns nullptr if unknown.
    wil::com_ptr<IWSLCProcess> FindProcess(DWORD cookie) const;

    // Remove a cookie mapping; returns the removed process (may be null).
    wil::com_ptr<IWSLCProcess> RemoveProcess(DWORD cookie);

    mutable std::mutex m_processLock;
    std::unordered_map<DWORD, wil::com_ptr<IWSLCProcess>> m_processes;
    DWORD m_nextCookie{1};

    // The PluginManager that owns this callback. Used to resolve a WSLC
    // SessionId to a live IWSLCSession via the manager's registered session
    // reference map (see PluginManager::ResolveWslcSession).
    PluginManager& m_owner;
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

    // Releases all out-of-process plugin host COM proxies and tears down the
    // job object that contains the host processes. MUST be called from the
    // service shutdown path WHILE COM IS STILL INITIALIZED on this thread
    // (i.e. before CoUninitialize). Releasing IWslPluginHost proxies after
    // COM has been uninitialized — for example, when this PluginManager is
    // destroyed as a global at process exit — crashes inside the marshaler
    // because the proxy/stub DLL has been unloaded.
    void Shutdown();

    void OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings);
    void OnVmStopping(const WSLSessionInformation* Session);
    void OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* distro);
    void OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* distro);
    void OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro);
    void OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* distro);

    // WSLC notifications. Returning failure from OnWslcSessionCreated / OnWslcContainerStarted causes
    // the corresponding operation to be aborted. Other notifications log errors and continue.
    void OnWslcSessionCreated(const WSLCSessionInformation* Session);
    void OnWslcSessionStopping(const WSLCSessionInformation* Session);
    HRESULT OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson);
    void OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId);
    void OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson);
    void OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId);

    void ThrowIfFatalPluginError();

    // WSLC session reference registry. The WSLCSessionManager registers a weak
    // IWSLCSessionReference for each live session here (keyed by SessionId)
    // before notifying plugins of the session, and unregisters it when the
    // session stops or creation is rolled back. Plugin API callbacks that take
    // a SessionId (WslcMountFolder/WslcCreateProcess/etc.) resolve the live
    // session through this registry instead of reaching back into the
    // WSLCSessionManager — this breaks the lock-reentrancy cycle that would
    // otherwise occur when an out-of-process plugin calls back on a different
    // RPC thread while the manager holds its session lock.
    void RegisterWslcSession(ULONG SessionId, IWSLCSessionReference* Reference);
    void UnregisterWslcSession(ULONG SessionId);

    // Resolves a SessionId to a live IWSLCSession. The reference is copied out
    // under m_wslcSessionRefLock and OpenSession() is then called OUTSIDE the
    // lock (the reference is a weak handle, so OpenSession can fail/block).
    // Throws HRESULT_FROM_WIN32(ERROR_NOT_FOUND) if the session is unknown or
    // can no longer be opened.
    wil::com_ptr<IWSLCSession> ResolveWslcSession(ULONG SessionId);

private:
    struct OutOfProcPlugin
    {
        // GIT cookie for the IWslPluginHost proxy. Zero means "not loaded".
        // Resolved per-call via AcquireHostProxy() because the raw proxy
        // returned by CoCreateInstance is apartment-bound to the LoadPlugin
        // thread, but hook dispatch can arrive on threads in any apartment
        // (MTA, NTA-on-MTA via WinRT-style RPC dispatch, etc.). Storing in
        // the Global Interface Table and re-unmarshaling per call yields an
        // apartment-local proxy that COM can dispatch from any apartment.
        DWORD hostCookie{0};
        Microsoft::WRL::ComPtr<PluginHostCallbackImpl> callback;
        std::wstring name;
        std::wstring path;

        // Set the first time a host crash is observed for this plugin during
        // runtime (i.e. after successful load). Subsequent crash sites only
        // log to ETL and skip the telemetry event, avoiding flood from a
        // dead plugin that we'd notify on every distro/VM lifecycle event.
        std::atomic<bool> crashTelemetryFired{false};

        OutOfProcPlugin() = default;
        OutOfProcPlugin(const OutOfProcPlugin&) = delete;
        OutOfProcPlugin& operator=(const OutOfProcPlugin&) = delete;
        OutOfProcPlugin(OutOfProcPlugin&& other) noexcept :
            hostCookie(std::exchange(other.hostCookie, 0)),
            callback(std::move(other.callback)),
            name(std::move(other.name)),
            path(std::move(other.path)),
            crashTelemetryFired(other.crashTelemetryFired.load())
        {
        }
        OutOfProcPlugin& operator=(OutOfProcPlugin&&) = delete;
    };

    // RAII helper that joins the calling thread to the MTA for the duration of the
    // scope. Plugin host activation and per-call dispatch through plugin host proxies
    // are both cross-process COM calls that require the calling thread to be COM-init'd,
    // and hook dispatch can run on threadpool threads (e.g. _VmIdleTerminate path) that
    // haven't called CoInitializeEx. RPC_E_CHANGED_MODE means the thread is already
    // initialized to a different apartment (typically STA), which is still fine: the
    // existing apartment is preserved and COM dispatches proxy calls accordingly.
    struct ScopedComInit
    {
        HRESULT initHr{RPC_E_CHANGED_MODE};

        ScopedComInit();
        ~ScopedComInit();
        ScopedComInit(const ScopedComInit&) = delete;
        ScopedComInit& operator=(const ScopedComInit&) = delete;
        ScopedComInit(ScopedComInit&& other) noexcept;
        ScopedComInit& operator=(ScopedComInit&&) = delete;

        HRESULT Result() const noexcept;
    };

    void LoadPlugin(OutOfProcPlugin& plugin);
    [[nodiscard]] ScopedComInit EnsureInitialized();
    void EnsureJobObjectCreated();

    // Returns an apartment-local IWslPluginHost proxy for the given plugin by
    // re-unmarshaling from the Global Interface Table. The returned proxy is
    // only valid on the current thread / apartment and must not be cached.
    // On failure (host process crashed, host released, etc.) returns the
    // HRESULT from GIT; caller should treat host-crash HRESULTs via
    // IsHostCrash() exactly as if the call itself had failed.
    HRESULT AcquireHostProxy(const OutOfProcPlugin& plugin, _COM_Outptr_ IWslPluginHost** host);

    static void ThrowIfPluginError(HRESULT Result, LPWSTR ErrorMessage, WSLSessionId session, LPCWSTR Plugin);
    static std::vector<BYTE> SerializeSid(PSID Sid);
    static bool IsHostCrash(HRESULT hr);

    // Logs a host crash to ETL and fires the PluginHostCrash telemetry event
    // at most once per plugin per service lifetime, so a single bad plugin
    // does not flood telemetry across every subsequent VM/distro lifecycle.
    static void LogPluginHostCrash(OutOfProcPlugin& plugin, HRESULT result, const char* stage);

    std::once_flag m_initOnce;
    std::vector<OutOfProcPlugin> m_plugins;
    std::optional<PluginError> m_pluginError;

    // Global Interface Table used to make IWslPluginHost proxies callable from
    // any apartment. Created lazily inside EnsureInitialized() while COM is
    // guaranteed to be initialized on the calling thread.
    std::once_flag m_gitOnce;
    Microsoft::WRL::ComPtr<IGlobalInterfaceTable> m_git;

    // Job object that automatically terminates all plugin host processes
    // when wslservice exits or crashes (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
    std::once_flag m_jobObjectOnce;
    wil::unique_handle m_jobObject;

    // Maps a WSLC SessionId to a weak reference to the live session. Populated
    // by RegisterWslcSession / drained by UnregisterWslcSession and Shutdown.
    // The references are COM proxies and MUST be released while COM is still
    // initialized (see Shutdown); the destructor detaches (leaks) any survivors
    // rather than releasing them after CoUninitialize.
    std::mutex m_wslcSessionRefLock;
    std::unordered_map<ULONG, wil::com_ptr<IWSLCSessionReference>> m_wslcSessionRefs;
};

} // namespace wsl::windows::service
