/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginManager.cpp

Abstract:

    This file contains the PluginManager helper class implementation.

--*/

#include "precomp.h"
#include "install.h"
#include "PluginManager.h"
#include "WslPluginApi.h"
#include "LxssUserSessionFactory.h"
#include "WSLCSessionManager.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::service::PluginManager;

constexpr auto c_pluginPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins";

constexpr WSLVersion Version = {wsl::shared::VersionMajor, wsl::shared::VersionMinor, wsl::shared::VersionRevision};

thread_local std::optional<std::wstring> g_pluginErrorMessage;

extern "C" {
HRESULT MountFolder(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name)
try
{
    const auto session = FindSessionByCookie(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    auto result = session->MountRootNamespaceFolder(WindowsPath, LinuxPath, ReadOnly, Name);

    WSL_LOG(
        "PluginMountFolderCall",
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(LinuxPath, "LinuxPath"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Name, "Name"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT ExecuteBinary(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
try
{

    const auto session = FindSessionByCookie(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    auto result = session->CreateLinuxProcess(nullptr, Path, Arguments, Socket);

    WSL_LOG("PluginExecuteBinaryCall", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));
    return result;
}
CATCH_RETURN();

HRESULT PluginError(LPCWSTR UserMessage)
try
{
    const auto* context = ExecutionContext::Current();
    THROW_HR_IF(E_INVALIDARG, UserMessage == nullptr);
    THROW_HR_IF_MSG(
        E_ILLEGAL_METHOD_CALL, context == nullptr || WI_IsFlagClear(context->CurrentContext(), Context::Plugin), "Message: %ls", UserMessage);

    // Logs when a WSL plugin hits an error and what that error message is
    WSL_LOG_TELEMETRY("PluginError", PDT_ProductAndServicePerformance, TraceLoggingValue(UserMessage, "Message"));

    THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, g_pluginErrorMessage.has_value());

    g_pluginErrorMessage.emplace(UserMessage);

    return S_OK;
}
CATCH_RETURN();

HRESULT ExecuteBinaryInDistribution(WSLSessionId Session, const GUID* Distro, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket)
try
{
    THROW_HR_IF(E_INVALIDARG, Distro == nullptr);

    const auto session = FindSessionByCookie(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    auto result = session->CreateLinuxProcess(Distro, Path, Arguments, Socket);

    WSL_LOG("PluginExecuteBinaryInDistributionCall", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();
}

namespace {

// Tracks WSLC processes created by plugins via WSLCPluginAPI_CreateProcess so that
// the corresponding WSLCPluginAPI_WaitPid call can resolve them by (session, pid).
struct WslcProcessKey
{
    ::WSLCSessionId Session;
    LONG Pid;
    auto operator<=>(const WslcProcessKey&) const = default;
};

std::mutex g_wslcProcessesMutex;
std::map<WslcProcessKey, wil::com_ptr<IWSLCProcess>> g_wslcProcesses;

std::optional<wsl::windows::service::wslc::WSLCSessionManagerImpl::ResolvedSession> ResolveWslcSession(WSLCSessionId Session)
{
    auto* mgr = wsl::windows::service::wslc::WSLCSessionManagerImpl::Instance();
    if (mgr == nullptr)
    {
        return std::nullopt;
    }

    return mgr->FindSession(static_cast<ULONG>(Session));
}

} // namespace

extern "C" {

HRESULT WSLCMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, BOOL ReadOnly, LPCWSTR Name, LPWSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || Name == nullptr || Mountpoint == nullptr);

    auto resolved = ResolveWslcSession(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !resolved.has_value());

    // Mount the folder under /mnt/wsl-plugin/<Name>. Convert Name to UTF-8 for the Linux path.
    const auto nameUtf8 = wsl::shared::string::WideToMultiByte(Name);
    const auto linuxPath = std::format("/mnt/wsl-plugin/{}", nameUtf8);

    auto result = resolved->Session->MountWindowsFolder(WindowsPath, linuxPath.c_str(), ReadOnly);

    WSL_LOG(
        "WslcPluginMountFolderCall",
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(linuxPath.c_str(), "LinuxPath"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Name, "Name"),
        TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        const auto wideLinuxPath = wsl::shared::string::MultiByteToWide(linuxPath);
        wcsncpy_s(Mountpoint, 256, wideLinuxPath.c_str(), _TRUNCATE);
    }

    return result;
}
CATCH_RETURN();

HRESULT WSLCUnmountFolder(WSLCSessionId Session, LPCWSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    auto resolved = ResolveWslcSession(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !resolved.has_value());

    const auto mountpointUtf8 = wsl::shared::string::WideToMultiByte(Mountpoint);
    auto result = resolved->Session->UnmountWindowsFolder(mountpointUtf8.c_str());

    WSL_LOG(
        "WslcPluginUnmountFolderCall",
        TraceLoggingValue(Mountpoint, "Mountpoint"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT WSLCCreateProcess(WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcess* Process)
try
{
    RETURN_HR_IF(E_POINTER, Executable == nullptr || Process == nullptr);

    auto resolved = ResolveWslcSession(Session);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !resolved.has_value());

    // Count NULL-terminated arrays.
    auto countArray = [](LPCSTR* arr) -> ULONG {
        if (arr == nullptr)
        {
            return 0;
        }
        ULONG count = 0;
        while (arr[count] != nullptr)
        {
            ++count;
        }
        return count;
    };

    WSLCProcessOptions options{};
    options.CommandLine.Values = Arguments;
    options.CommandLine.Count = countArray(Arguments);
    options.Environment.Values = Env;
    options.Environment.Count = countArray(Env);

    wil::com_ptr<IWSLCProcess> process;
    int errnoValue = 0;
    auto result = resolved->Session->CreateRootNamespaceProcess(Executable, &options, &process, &errnoValue);
    if (FAILED(result))
    {
        WSL_LOG(
            "WslcPluginCreateProcessCall",
            TraceLoggingValue(Executable, "Executable"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(errnoValue, "Errno"));
        return result;
    }

    int pid = 0;
    THROW_IF_FAILED(process->GetPid(&pid));

    // Retrieve std handles + exit event. Each must be SOCKET-typed by the plugin contract.
    auto getSocket = [&](WSLCFD fd, SOCKET& out) -> HRESULT {
        WSLCHandle handle{};
        RETURN_IF_FAILED(process->GetStdHandle(fd, &handle));
        if (handle.Type != WSLCHandleTypeSocket)
        {
            return E_UNEXPECTED;
        }
        out = reinterpret_cast<SOCKET>(handle.Handle.Socket);
        return S_OK;
    };

    SOCKET stdinSock = INVALID_SOCKET;
    SOCKET stdoutSock = INVALID_SOCKET;
    SOCKET stderrSock = INVALID_SOCKET;
    HANDLE exitEvent = nullptr;
    THROW_IF_FAILED(getSocket(WSLCFDStdin, stdinSock));
    THROW_IF_FAILED(getSocket(WSLCFDStdout, stdoutSock));
    THROW_IF_FAILED(getSocket(WSLCFDStderr, stderrSock));
    THROW_IF_FAILED(process->GetExitEvent(&exitEvent));

    {
        std::lock_guard lock(g_wslcProcessesMutex);
        g_wslcProcesses[{Session, pid}] = process;
    }

    Process->Pid = static_cast<ULONG>(pid);
    Process->Stdin = stdinSock;
    Process->Stdout = stdoutSock;
    Process->Stderr = stderrSock;
    Process->ExitEvent = exitEvent;

    WSL_LOG(
        "WslcPluginCreateProcessCall",
        TraceLoggingValue(Executable, "Executable"),
        TraceLoggingValue(pid, "Pid"),
        TraceLoggingValue(S_OK, "Result"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCWaitPid(WSLCSessionId Session, LONG Pid, ULONGLONG Timeout, int* Status)
try
{
    RETURN_HR_IF(E_POINTER, Status == nullptr);

    wil::com_ptr<IWSLCProcess> process;
    {
        std::lock_guard lock(g_wslcProcessesMutex);
        auto it = g_wslcProcesses.find({Session, Pid});
        if (it == g_wslcProcesses.end())
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        process = it->second;
    }

    wil::unique_handle exitEvent;
    THROW_IF_FAILED(process->GetExitEvent(exitEvent.put()));

    const DWORD timeoutMs = (Timeout == 0 || Timeout > MAXDWORD) ? INFINITE : static_cast<DWORD>(Timeout);
    const auto waitResult = WaitForSingleObject(exitEvent.get(), timeoutMs);
    if (waitResult == WAIT_TIMEOUT)
    {
        WSL_LOG("WslcPluginWaitPidCall", TraceLoggingValue(Pid, "Pid"), TraceLoggingValue(HRESULT_FROM_WIN32(WAIT_TIMEOUT), "Result"));
        return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    THROW_LAST_ERROR_IF(waitResult != WAIT_OBJECT_0);

    WSLCProcessState state{};
    int code = 0;
    THROW_IF_FAILED(process->GetState(&state, &code));
    *Status = code;

    {
        std::lock_guard lock(g_wslcProcessesMutex);
        g_wslcProcesses.erase({Session, Pid});
    }

    WSL_LOG(
        "WslcPluginWaitPidCall",
        TraceLoggingValue(Pid, "Pid"),
        TraceLoggingValue(code, "Status"),
        TraceLoggingValue(S_OK, "Result"));

    return S_OK;
}
CATCH_RETURN();

} // extern "C"

static constexpr WSLCPluginAPIV1 WslcApiV1 = {&WSLCMountFolder, &WSLCUnmountFolder, &WSLCCreateProcess, &WSLCWaitPid};

static constexpr WSLPluginAPIV1 ApiV1 = {Version, &MountFolder, &ExecuteBinary, &PluginError, &ExecuteBinaryInDistribution, &WslcApiV1};

void PluginManager::LoadPlugins()
{
    ExecutionContext context(Context::Plugin);

    const auto key = common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_pluginPath, KEY_READ);
    const auto values = common::registry::EnumValues(key.get());

    std::set<std::wstring, wsl::shared::string::CaseInsensitiveCompare> loaded;
    for (const auto& e : values)
    {
        if (e.second != REG_SZ)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Plugin value: '%ls' has incorrect type: %lu, skipping", e.first.c_str(), e.second);
            continue;
        }

        auto path = common::registry::ReadString(key.get(), nullptr, e.first.c_str());

        if (!loaded.insert(path).second)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Module '%ls' has already been loaded, skipping plugin '%ls'", path.c_str(), e.first.c_str());
            continue;
        }

        auto loadResult = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&]() { LoadPlugin(e.first.c_str(), path.c_str()); });

        // Logs when a WSL plugin is loaded, used for evaluating plugin populations
        WSL_LOG_TELEMETRY(
            "PluginLoad",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(e.first.c_str(), "Name"),
            TraceLoggingValue(path.c_str(), "Path"),
            TraceLoggingValue(loadResult, "Result"));

        if (FAILED(loadResult))
        {
            // If this plugin reported an error, record it to display it to the user
            m_pluginError.emplace(PluginError{e.first, loadResult});
        }
    }
}

void PluginManager::LoadPlugin(LPCWSTR Name, LPCWSTR ModulePath)
{
    // Validate the plugin signature before loading it.
    // The handle to the module is kept open after validating the signature so the file can't be written to
    // after the signature check.
    wil::unique_hfile pluginHandle;
    if constexpr (wsl::shared::OfficialBuild)
    {
        pluginHandle = wsl::windows::common::install::ValidateFileSignature(ModulePath);
        WI_ASSERT(pluginHandle.is_valid());
    }

    LoadedPlugin plugin{};
    plugin.name = Name;

    plugin.module.reset(LoadLibrary(ModulePath));
    THROW_LAST_ERROR_IF_NULL(plugin.module);

    const WSLPluginAPI_EntryPointV1 entryPoint =
        reinterpret_cast<WSLPluginAPI_EntryPointV1>(GetProcAddress(plugin.module.get(), GSL_STRINGIFY(WSLPLUGINAPI_ENTRYPOINTV1)));

    THROW_LAST_ERROR_IF_NULL(entryPoint);
    THROW_IF_FAILED_MSG(entryPoint(&ApiV1, &plugin.hooks), "Error returned by plugin: '%ls'", ModulePath);

    m_plugins.emplace_back(std::move(plugin));
}

void PluginManager::OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnVMStarted != nullptr)
        {
            WSL_LOG(
                "PluginOnVmStartedCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

            ThrowIfPluginError(e.hooks.OnVMStarted(Session, Settings), Session->SessionId, e.name.c_str());
        }
    }
}

void PluginManager::OnVmStopping(const WSLSessionInformation* Session) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnVMStopping != nullptr)
        {
            WSL_LOG(
                "PluginOnVmStoppingCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

            const auto result = e.hooks.OnVMStopping(Session);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnDistributionStarted != nullptr)
        {
            WSL_LOG(
                "PluginOnDistroStartedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->UserSid, "Sid"),
                TraceLoggingValue(Distribution->Id, "DistributionId"));

            ThrowIfPluginError(e.hooks.OnDistributionStarted(Session, Distribution), Session->SessionId, e.name.c_str());
        }
    }
}

void PluginManager::OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnDistributionStopping != nullptr)
        {
            WSL_LOG(
                "PluginOnDistroStoppingCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->UserSid, "Sid"),
                TraceLoggingValue(Distribution->Id, "DistributionId"));

            const auto result = e.hooks.OnDistributionStopping(Session, Distribution);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnDistributionRegistered != nullptr)
        {
            WSL_LOG(
                "PluginOnDistributionRegisteredCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->UserSid, "Sid"),
                TraceLoggingValue(Distribution->Id, "DistributionId"));

            const auto result = e.hooks.OnDistributionRegistered(Session, Distribution);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnDistributionUnregistered != nullptr)
        {
            WSL_LOG(
                "PluginOnDistributionUnregisteredCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->UserSid, "Sid"),
                TraceLoggingValue(Distribution->Id, "DistributionId"));

            const auto result = e.hooks.OnDistributionUnregistered(Session, Distribution);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::ThrowIfPluginError(HRESULT Result, WSLSessionId Session, LPCWSTR Plugin)
{
    const auto message = std::move(g_pluginErrorMessage);
    g_pluginErrorMessage.reset(); // std::move() doesn't clear the previous std::optional

    if (FAILED(Result))
    {
        if (message.has_value())
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginErrorWithMessage(Plugin, message->c_str()));
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginError(Plugin));
        }
    }
    else if (message.has_value())
    {
        THROW_HR_MSG(E_ILLEGAL_STATE_CHANGE, "Plugin '%ls' emitted an error message but returned success", Plugin);
    }
}

void PluginManager::ThrowIfFatalPluginError() const
{
    ExecutionContext context(Context::Plugin);

    if (!m_pluginError.has_value())
    {
        return;
    }
    else if (m_pluginError->error == WSL_E_PLUGIN_REQUIRES_UPDATE)
    {
        THROW_HR_WITH_USER_ERROR(
            WSL_E_PLUGIN_REQUIRES_UPDATE, wsl::shared::Localization::MessagePluginRequiresUpdate(m_pluginError->plugin));
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(m_pluginError->error, wsl::shared::Localization::MessageFatalPluginError(m_pluginError->plugin));
    }
}

void PluginManager::OnWslcSessionCreated(const WSLCSessionInformation* Session)
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnSessionCreated != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcSessionCreatedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(Session->DisplayName, "DisplayName"));

            const auto result = e.hooks.OnSessionCreated(Session);
            // Reuse ThrowIfPluginError for the user-error / fatal-error semantics.
            // SessionId on the WSL plugin error path expects WSLSessionId; reuse 0 since this is a WSLC session.
            ThrowIfPluginError(result, 0, e.name.c_str());
        }
    }
}

void PluginManager::OnWslcSessionStopping(const WSLCSessionInformation* Session) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.OnSessionStopping != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcSessionStoppingCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"));

            const auto result = e.hooks.OnSessionStopping(Session);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

HRESULT PluginManager::OnWslcContainerStarted(const WSLCSessionInformation* Session, LPCSTR InspectJson) const
try
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ContainerStarted != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcContainerStartedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"));

            // Failure here aborts the container creation. Surface the first error.
            const auto result = e.hooks.ContainerStarted(Session, InspectJson);
            if (FAILED(result))
            {
                LOG_HR_MSG(result, "Plugin '%ls' rejected container creation", e.name.c_str());
                return result;
            }
        }
    }
    return S_OK;
}
CATCH_RETURN()

void PluginManager::OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR InspectJson) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ContainerStopping != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcContainerStoppingCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"));

            const auto result = e.hooks.ContainerStopping(Session, InspectJson);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnWslcImageCreated(const WSLCSessionInformation* Session, LPCSTR InspectJson) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ImageCreated != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcImageCreatedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"));

            const auto result = e.hooks.ImageCreated(Session, InspectJson);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR InspectJson) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ImageDeleted != nullptr)
        {
            WSL_LOG(
                "PluginOnWslcImageDeletedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"));

            const auto result = e.hooks.ImageDeleted(Session, InspectJson);
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}
