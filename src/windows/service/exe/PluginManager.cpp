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

// Opaque wrapper around IWSLCProcess, handed out as WSLCProcessHandle to plugins.
struct WslcProcessWrapper
{
    wil::com_ptr<IWSLCProcess> Process;
};

wil::com_ptr<IWSLCSession> ResolveWslcSession(WSLCSessionId Session)
{
    auto* mgr = wsl::windows::service::wslc::WSLCSessionManagerImpl::Instance();
    THROW_HR_IF(RPC_E_DISCONNECTED, mgr == nullptr);

    return mgr->FindSession(static_cast<ULONG>(Session));
}

} // namespace

extern "C" {

HRESULT WSLCMountFolder(WSLCSessionId Session, LPCWSTR WindowsPath, BOOL ReadOnly, LPCWSTR Name, LPSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || Name == nullptr || Mountpoint == nullptr);
    auto nameLength = wcslen(Name);

    RETURN_HR_IF_MSG(
        E_INVALIDARG,
        nameLength == 0 ||
            !std::ranges::all_of(Name, Name + nameLength, [&](auto c) { return c == '-' || c == '_' || iswalnum(c); }),
        "Invalid mount name: %ls",
        Name);

    auto session = ResolveWslcSession(Session);

    // Mount the folder under /mnt/wsl-plugin/<Name>. Convert Name to UTF-8 for the Linux path.
    const auto linuxPath = std::format("/mnt/wsl-plugin/{}", Name);

    THROW_HR_IF_MSG(E_INVALIDARG, linuxPath.length() >= WSLC_MOUNTPOINT_LENGTH, "Mountpoint too long: %hs", linuxPath.c_str());

    auto result = session->MountWindowsFolder(WindowsPath, linuxPath.c_str(), ReadOnly);

    WSL_LOG(
        "WslcPluginMountFolderCall",
        TraceLoggingValue(Session, "SessionId"),
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(linuxPath.c_str(), "LinuxPath"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Name, "Name"),
        TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        THROW_HR_IF(E_UNEXPECTED, strcpy_s(Mountpoint, WSLC_MOUNTPOINT_LENGTH, linuxPath.c_str()) != 0);
    }

    return result;
}
CATCH_RETURN();

HRESULT WSLCUnmountFolder(WSLCSessionId Session, LPCSTR Mountpoint)
try
{
    RETURN_HR_IF(E_POINTER, Mountpoint == nullptr);

    auto session = ResolveWslcSession(Session);

    auto result = session->UnmountWindowsFolder(Mountpoint);

    WSL_LOG(
        "WslcPluginUnmountFolderCall",
        TraceLoggingValue(Session, "SessionId"),
        TraceLoggingValue(Mountpoint, "Mountpoint"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT WSLCCreateProcess(WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, WSLCProcessHandle* Process, int* Errno)
try
{
    RETURN_HR_IF(E_POINTER, Executable == nullptr || Process == nullptr);

    *Process = nullptr;
    if (Errno != nullptr)
    {
        *Errno = 0;
    }

    auto session = ResolveWslcSession(Session);

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
    options.Flags = WSLCProcessFlagsStdin;

    wil::com_ptr<IWSLCProcess> process;
    int errnoValue = 0;
    auto result = session->CreateRootNamespaceProcess(Executable, &options, &process, &errnoValue);

    if (Errno != nullptr)
    {
        *Errno = errnoValue;
    }

    if (FAILED(result))
    {
        WSL_LOG(
            "WslcPluginCreateProcessCall",
            TraceLoggingValue(Session, "SessionId"),
            TraceLoggingValue(Executable, "Executable"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(errnoValue, "Errno"));
        return result;
    }

    auto wrapper = std::make_unique<WslcProcessWrapper>();
    wrapper->Process = std::move(process);
    *Process = wrapper.release();

    WSL_LOG(
        "WslcPluginCreateProcessCall",
        TraceLoggingValue(Session, "SessionId"),
        TraceLoggingValue(Executable, "Executable"),
        TraceLoggingValue(*Process, "Process"),
        TraceLoggingValue(S_OK, "Result"));

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcessGetFd(WSLCProcessHandle Process, WSLCProcessFd Fd, HANDLE* Handle)
try
{
    RETURN_HR_IF(E_POINTER, Process == nullptr || Handle == nullptr);

    *Handle = nullptr;

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);

    WSLCFD wslcFd{};
    switch (Fd)
    {
    case WSLCProcessFdStdin:
        wslcFd = WSLCFDStdin;
        break;
    case WSLCProcessFdStdout:
        wslcFd = WSLCFDStdout;
        break;
    case WSLCProcessFdStderr:
        wslcFd = WSLCFDStderr;
        break;
    default:
        WSL_LOG(
            "WslcPluginProcessGetFd", TraceLoggingValue(static_cast<int>(Fd), "Fd"), TraceLoggingValue(E_INVALIDARG, "Result"));
        return E_INVALIDARG;
    }

    WSLCHandle handle{};
    auto result = wrapper->Process->GetStdHandle(wslcFd, &handle);

    WSL_LOG(
        "WslcPluginProcessGetFd",
        TraceLoggingValue(static_cast<int>(Fd), "Fd"),
        TraceLoggingValue(handle.Handle.Socket, "Handle"),
        TraceLoggingValue(result, "Result"));

    RETURN_IF_FAILED(result);
    WI_ASSERT(handle.Type == WSLCHandleTypeSocket);

    *Handle = handle.Handle.Socket;
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCProcessGetExitEvent(WSLCProcessHandle Process, HANDLE* ExitEvent)
try
{
    RETURN_HR_IF(E_POINTER, Process == nullptr || ExitEvent == nullptr);

    *ExitEvent = nullptr;

    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);
    auto result = wrapper->Process->GetExitEvent(ExitEvent);

    WSL_LOG("WslcPluginProcessGetExitEvent", TraceLoggingValue(*ExitEvent, "ExitEvent"), TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT WSLCProcessGetExitCode(WSLCProcessHandle Process, int* ExitCode)
try
{
    RETURN_HR_IF(E_POINTER, Process == nullptr || ExitCode == nullptr);

    *ExitCode = -1;
    auto* wrapper = static_cast<WslcProcessWrapper*>(Process);

    WSLCProcessState state{};
    auto result = wrapper->Process->GetState(&state, ExitCode);

    if (SUCCEEDED(result) && state != WslcProcessStateExited && state != WslcProcessStateSignalled)
    {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }

    WSL_LOG(
        "WslcPluginProcessGetExitCode",
        TraceLoggingValue(*ExitCode, "ExitCode"),
        TraceLoggingValue(static_cast<int>(state), "State"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

void WSLCReleaseProcess(WSLCProcessHandle Process)
{
    if (Process != nullptr)
    {
        WSL_LOG("WslcPluginReleaseProcess", TraceLoggingValue(Process, "Process"));
        delete static_cast<WslcProcessWrapper*>(Process);
    }
}

} // extern "C"

static constexpr WSLPluginAPIV1 ApiV1 = {
    Version,
    &MountFolder,
    &ExecuteBinary,
    &PluginError,
    &ExecuteBinaryInDistribution,
    &WSLCMountFolder,
    &WSLCUnmountFolder,
    &WSLCCreateProcess,
    &WSLCProcessGetFd,
    &WSLCProcessGetExitEvent,
    &WSLCProcessGetExitCode,
    &WSLCReleaseProcess};

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

            ThrowIfPluginError(e.hooks.OnVMStarted(Session, Settings), e.name.c_str());
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

            ThrowIfPluginError(e.hooks.OnDistributionStarted(Session, Distribution), e.name.c_str());
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

void PluginManager::ThrowIfPluginError(HRESULT Result, LPCWSTR Plugin)
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
            auto result = e.hooks.OnSessionCreated(Session);
            WSL_LOG(
                "PluginOnWslcSessionCreatedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(Session->DisplayName, "DisplayName"),
                TraceLoggingValue(result, "Result"));

            ThrowIfPluginError(result, e.name.c_str());
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
            const auto result = e.hooks.OnSessionStopping(Session);
            WSL_LOG(
                "PluginOnWslcSessionStoppingCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(result, "Result"));

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
            // Failure here aborts the container creation. Surface the first error.
            const auto result = e.hooks.ContainerStarted(Session, InspectJson);
            WSL_LOG(
                "PluginOnWslcContainerStartedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(result, "Result"));

            ThrowIfPluginError(result, e.name.c_str());
        }
    }
    return S_OK;
}
CATCH_RETURN()

void PluginManager::OnWslcContainerStopping(const WSLCSessionInformation* Session, LPCSTR ContainerId) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ContainerStopping != nullptr)
        {

            const auto result = e.hooks.ContainerStopping(Session, ContainerId);
            WSL_LOG(
                "PluginOnWslcContainerStoppingCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(ContainerId, "ContainerId"),
                TraceLoggingValue(result, "Result"));

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
            const auto result = e.hooks.ImageCreated(Session, InspectJson);
            WSL_LOG(
                "PluginOnWslcImageCreatedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(result, "Result"));

            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}

void PluginManager::OnWslcImageDeleted(const WSLCSessionInformation* Session, LPCSTR ImageId) const
{
    ExecutionContext context(Context::Plugin);

    for (const auto& e : m_plugins)
    {
        if (e.hooks.ImageDeleted != nullptr)
        {
            const auto result = e.hooks.ImageDeleted(Session, ImageId);
            WSL_LOG(
                "PluginOnWslcImageDeletedCall",
                TraceLoggingValue(e.name.c_str(), "Plugin"),
                TraceLoggingValue(Session->SessionId, "SessionId"),
                TraceLoggingValue(ImageId, "ImageId"),
                TraceLoggingValue(result, "Result"));
            LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
        }
    }
}
