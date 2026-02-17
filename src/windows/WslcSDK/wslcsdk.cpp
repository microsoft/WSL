/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslcsdk.cpp

Abstract:

    This file contains the public WSLC Client SDK api implementations.

--*/
#include "precomp.h"

#include "wslcsdk.h"
#include "WslcsdkPrivate.h"
#include "ProgressCallback.h"
#include "TerminationCallback.h"
#include "wslutil.h"

namespace {
constexpr uint32_t s_DefaultCPUCount = 2;
constexpr uint32_t s_DefaultMemoryMB = 2000;
// Maximum value per use with HVSOCKET_CONNECT_TIMEOUT_MAX
constexpr ULONG s_DefaultBootTimeout = 300000;
// Default to 1 GB
constexpr UINT64 s_DefaultStorageSize = 1000 * 1000 * 1000;

WSLAFeatureFlags ConvertFlags(WslcSessionFlags flags)
{
    static_assert(WSLC_SESSION_FLAG_ENABLE_GPU == WslaFeatureFlagsGPU, "Session GPU flag values differ.");

    WslcSessionFlags allFlagsMask = WSLC_SESSION_FLAG_ENABLE_GPU;

    return static_cast<WSLAFeatureFlags>(flags & allFlagsMask);
}

WSLAContainerFlags ConvertFlags(WslcContainerFlags flags)
{
    static_assert(WSLC_CONTAINER_FLAG_AUTO_REMOVE == WSLAContainerFlagsRm, "Container auto remove flag values differ.");
    static_assert(WSLC_CONTAINER_FLAG_ENABLE_GPU == WSLAContainerFlagsGpu, "Container GPU flag values differ.");
    // TODO: Are these the same flags?
    // static_assert(WSLC_CONTAINER_FLAG_PRIVILEGED == WSLAContainerFlagsInit, "Container privileged flag values differ.");

    WslcContainerFlags allFlagsMask = WSLC_CONTAINER_FLAG_AUTO_REMOVE | WSLC_CONTAINER_FLAG_ENABLE_GPU;

    return static_cast<WSLAContainerFlags>(flags & allFlagsMask);
}

WSLASignal ConvertSignal(WslcSignal signal)
{
    switch (signal)
    {
    case WSLC_SIGNAL_NONE:
        return WSLASignal::WSLASignalNone;
    case WSLC_SIGNAL_SIGHUP:
        return WSLASignal::WSLASignalSIGHUP;
    case WSLC_SIGNAL_SIGINT:
        return WSLASignal::WSLASignalSIGINT;
    case WSLC_SIGNAL_SIGQUIT:
        return WSLASignal::WSLASignalSIGQUIT;
    case WSLC_SIGNAL_SIGKILL:
        return WSLASignal::WSLASignalSIGKILL;
    case WSLC_SIGNAL_SIGTERM:
        return WSLASignal::WSLASignalSIGTERM;
    default:
        THROW_HR(E_INVALIDARG);
    }
}

WSLANetworkingMode Convert(WslcSessionNetworkingMode mode)
{
    static_assert(WSLC_SESSION_NETWORKING_MODE_NONE == WSLANetworkingModeNone, "Session networking none values differ.");
    static_assert(WSLC_SESSION_NETWORKING_MODE_NAT == WSLANetworkingModeNAT, "Session networking NAT values differ.");
    static_assert(WSLC_SESSION_NETWORKING_MODE_VIRT_IO_PROXY == WSLANetworkingModeVirtioProxy, "Session networking Virt IO values differ.");

    THROW_HR_IF(E_INVALIDARG, !(WSLC_SESSION_NETWORKING_MODE_NONE <= mode && mode <= WSLC_SESSION_NETWORKING_MODE_VIRT_IO_PROXY));

    return static_cast<WSLANetworkingMode>(mode);
}

void GetErrorInfoFromCOM(PWSTR* errorMessage)
{
    if (errorMessage)
    {
        *errorMessage = nullptr;
        auto errorInfo = wsl::windows::common::wslutil::GetCOMErrorInfo();
        if (errorInfo)
        {
            *errorMessage = wil::make_unique_string<wil::unique_cotaskmem_string>(errorInfo->Message.get()).release();
        }
    }
}
} // namespace

// SESSION DEFINITIONS
STDAPI WslcSessionInitSettings(_In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    *internalType = {};

    internalType->storagePath = storagePath;
    internalType->cpuCount = s_DefaultCPUCount;
    internalType->memoryMb = s_DefaultMemoryMB;
    internalType->timeoutMS = s_DefaultBootTimeout;
    internalType->vhdRequirements.sizeInBytes = s_DefaultStorageSize;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (cpuCount)
    {
        internalType->cpuCount = cpuCount;
    }
    else
    {
        internalType->cpuCount = s_DefaultCPUCount;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMb)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (memoryMb)
    {
        internalType->memoryMb = memoryMb;
    }
    else
    {
        internalType->memoryMb = s_DefaultMemoryMB;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionCreate(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session)
try
{
    RETURN_HR_IF_NULL(E_POINTER, session);
    *session = nullptr;

    auto internalType = CheckAndGetInternalType(sessionSettings);

    wil::com_ptr<IWSLASessionManager> sessionManager;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    auto result = std::make_unique<WslcSessionImpl>();
    WSLA_SESSION_SETTINGS runtimeSettings{};
    runtimeSettings.DisplayName = internalType->displayName;
    runtimeSettings.StoragePath = internalType->storagePath;
    // TODO: Is this the intended use for vhdRequirements.sizeInBytes?
    runtimeSettings.MaximumStorageSizeMb = internalType->vhdRequirements.sizeInBytes / _1MB;
    runtimeSettings.CpuCount = internalType->cpuCount;
    runtimeSettings.MemoryMb = internalType->memoryMb;
    runtimeSettings.BootTimeoutMs = internalType->timeoutMS;
    runtimeSettings.NetworkingMode = internalType->networkingMode;
    auto terminationCallback = TerminationCallback::CreateIf(internalType);
    if (terminationCallback)
    {
        result->terminationCallback.attach(terminationCallback.as<ITerminationCallback>().detach());
        runtimeSettings.TerminationCallback = terminationCallback.get();
    }
    runtimeSettings.FeatureFlags = ConvertFlags(internalType->flags);

    // TODO: Debug message output? No user control? Expects a handle value as a ULONG (to write debug info to?)
    // runtimeSettings.DmesgOutput;

    // TODO: VHD overrides; I'm not sure if we intend these to be provided.
    // runtimeSettings.RootVhdOverride = internalType->vhdRequirements.path;
    // TODO: I don't think that this VHD type override can be reused from the VHD requirements type
    //       Tracking the code suggests that this is the `filesystemtype` to the linux `mount` function.
    //       Not clear how to map dynamic and fixed to values like `ext4` and `tmpfs`.
    // runtimeSettings.RootVhdTypeOverride = ConvertType(internalType->vhdRequirements.type);

    // TODO: No user control over flags (Persistent and OpenExisting)?
    RETURN_IF_FAILED(sessionManager->CreateSession(&runtimeSettings, WSLASessionFlagsNone, &result->session));
    wsl::windows::common::security::ConfigureForCOMImpersonation(result->session.get());

    *session = reinterpret_cast<WslcSession>(result.release());
    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionTerminate(_In_ WslcSession session)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);

    RETURN_HR(internalType->session->Terminate());
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode)
try
{
    UNREFERENCED_PARAMETER(networkingMode);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

// TODO: DisplayName is required (and is effectively `SessionKey`); it should be promoted to a required Init time parameter.
STDAPI WslcSessionSettingsSetDisplayName(_In_ WslcSessionSettings* sessionSettings, _In_ PCWSTR displayName)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    internalType->displayName = displayName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (timeoutMS)
    {
        internalType->timeoutMS = timeoutMS;
    }
    else
    {
        internalType->timeoutMS = s_DefaultBootTimeout;
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetNetworkingMode(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionNetworkingMode mode)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    internalType->networkingMode = Convert(mode);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionCreateVhd(_In_ WslcSession session, _In_ const WslcVhdRequirements* options)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetVHD(_In_ WslcSessionSettings* sessionSettings, _In_ const WslcVhdRequirements* vhdRequirements)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    if (vhdRequirements)
    {
        internalType->vhdRequirements = *vhdRequirements;
    }
    else
    {
        internalType->vhdRequirements = {};
    }

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName)
try
{
    UNREFERENCED_PARAMETER(hostName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName)
try
{
    UNREFERENCED_PARAMETER(domainName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);

    internalType->flags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionSettingsSetTerminateCallback(
    _In_ WslcSessionSettings* sessionSettings, _In_opt_ WslcSessionTerminationCallback terminationCallback, _In_opt_ PVOID terminationContext)
try
{
    auto internalType = CheckAndGetInternalType(sessionSettings);
    RETURN_HR_IF(E_INVALIDARG, terminationCallback == nullptr && terminationContext != nullptr);

    internalType->terminationCallback = terminationCallback;
    internalType->terminationCallbackContext = terminationContext;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcSessionRelease(_In_ WslcSession session)
try
{
    auto internalType = CheckAndGetInternalTypeUniquePointer(session);

    // Intentionally destroy session before termination callback in the event that
    // the termination callback ends up being invoked by session destruction.
    internalType->session.reset();
    internalType->terminationCallback.reset();

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerRelease(_In_ WslcContainer container)
try
{
    CheckAndGetInternalTypeUniquePointer(container);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessRelease(_In_ WslcProcess process)
try
{
    CheckAndGetInternalTypeUniquePointer(process);

    return S_OK;
}
CATCH_RETURN();

// CONTAINER DEFINITIONS

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);
    RETURN_HR_IF_NULL(E_POINTER, imageName);

    *internalType = {};

    internalType->image = imageName;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerCreate(_In_ WslcSession session, _In_ WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    RETURN_HR_IF_NULL(E_POINTER, container);
    *container = nullptr;

    auto internalSession = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalSession->session);
    auto internalContainerSettings = CheckAndGetInternalType(containerSettings);

    auto result = std::make_unique<WslcContainerImpl>();

    WSLA_CONTAINER_OPTIONS containerOptions{};
    containerOptions.Image = internalContainerSettings->image;
    containerOptions.Name = internalContainerSettings->runtimeName;
    containerOptions.HostName = internalContainerSettings->HostName;
    containerOptions.DomainName = internalContainerSettings->DomainName;
    containerOptions.Flags = ConvertFlags(internalContainerSettings->containerFlags);

    const WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* initProcessOptions = internalContainerSettings->initProcessOptions;
    if (initProcessOptions)
    {
        containerOptions.InitProcessOptions.CurrentDirectory = initProcessOptions->currentDirectory;
        containerOptions.InitProcessOptions.CommandLine.Values = initProcessOptions->commandLine;
        containerOptions.InitProcessOptions.CommandLine.Count = initProcessOptions->commandLineCount;
        containerOptions.InitProcessOptions.Environment.Values = initProcessOptions->environment;
        containerOptions.InitProcessOptions.Environment.Count = initProcessOptions->environmentCount;

        // TODO: No user access
        // containerOptions.InitProcessOptions.Flags;
        // containerOptions.InitProcessOptions.TtyRows;
        // containerOptions.InitProcessOptions.TtyColumns;
        // containerOptions.InitProcessOptions.User;
    }

    // TODO: Implement
    // containerOptions.Volumes;
    // containerOptions.VolumesCount;
    // containerOptions.Ports;
    // containerOptions.PortsCount;
    // containerOptions.ContainerNetwork;

    // TODO: No user access
    // containerOptions.Entrypoint;
    // containerOptions.Labels;
    // containerOptions.LabelsCount;
    // containerOptions.StopSignal;
    // containerOptions.ShmSize;

    HRESULT hr = internalSession->session->CreateContainer(&containerOptions, &result->container);

    if (FAILED_LOG(hr))
    {
        GetErrorInfoFromCOM(errorMessage);
    }
    else
    {
        wsl::windows::common::security::ConfigureForCOMImpersonation(result->container.get());
        *container = reinterpret_cast<WslcContainer>(result.release());
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcContainerStart(_In_ WslcContainer container)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    // TODO: No user choice between None and Attach (where attach is what allows access to init process IO handles)
    RETURN_HR(internalType->container->Start(WSLAContainerStartFlagsAttach));
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->containerFlags = flags;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->runtimeName = name;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess)
try
{
    auto internalType = CheckAndGetInternalType(containerSettings);

    internalType->initProcessOptions = GetInternalType(initProcess);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsSetPortMapping(
    _In_ WslcContainerSettings* containerSettings, _In_reads_(portMappingCount) const WslcContainerPortMapping* portMappings, _In_ uint32_t portMappingCount)
try
{
    UNREFERENCED_PARAMETER(portMappings);
    UNREFERENCED_PARAMETER(containerSettings);
    UNREFERENCED_PARAMETER(portMappingCount);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerSettingsAddVolume(_In_ WslcContainerSettings* containerSettings, _In_reads_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount)
try
{
    UNREFERENCED_PARAMETER(volumes);
    UNREFERENCED_PARAMETER(volumeCount);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerExec(_In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess)
try
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(newProcessSettings);
    UNREFERENCED_PARAMETER(newProcess);
    return E_NOTIMPL;
}
CATCH_RETURN();

// GENERAL CONTAINER MANAGEMENT

STDAPI WslcContainerGetID(WslcContainer container, PCHAR (*containerId)[WSLC_CONTAINER_ID_LENGTH])
try
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(containerId);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerInspect(_In_ WslcContainer container, _Outptr_result_z_ PCSTR* inspectData)
try
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(inspectData);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, initProcess);

    *initProcess = nullptr;

    auto result = std::make_unique<WslcProcessImpl>();

    RETURN_IF_FAILED(internalType->container->GetInitProcess(&result->process));

    *initProcess = reinterpret_cast<WslcProcess>(result.release());

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcContainerGetState(_In_ WslcContainer container, _Out_ WslcContainerState* state)
try
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcContainerStop(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutMS)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    // TODO: Resolve massive disparity between 32-bit unsigned millisecond input and 64-bit signed second target timeouts
    RETURN_HR(internalType->container->Stop(ConvertSignal(signal), timeoutMS / 1000));
}
CATCH_RETURN();

STDAPI WslcContainerDelete(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags)
try
{
    auto internalType = CheckAndGetInternalType(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    // TODO: Flags?
    UNREFERENCED_PARAMETER(flags);

    RETURN_HR(internalType->container->Delete());
}
CATCH_RETURN();

// PROCESS DEFINITIONS

STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    *internalType = {};

    return S_OK;
}
CATCH_RETURN();

// TODO: Executable has no place in runtime settings; it should be removed in favor of placement as the first item in CmdLineArgs.
STDAPI WslcProcessSettingsSetExecutable(_In_ WslcProcessSettings* processSettings, _In_ PCSTR executable)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);

    internalType->executable = executable;

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessSettingsSetCurrentDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR currentDirectory)
try
{
    UNREFERENCED_PARAMETER(currentDirectory);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsSetCmdLineArgs(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc)
try
{
    auto internalType = CheckAndGetInternalType(processSettings);
    RETURN_HR_IF(
        E_INVALIDARG,
        (argv == nullptr && argc != 0) || (argv != nullptr && argc == 0) ||
            (argc > static_cast<size_t>(std::numeric_limits<UINT32>::max())));

    internalType->commandLine = argv;
    internalType->commandLineCount = static_cast<UINT32>(argc);

    return S_OK;
}
CATCH_RETURN();

STDAPI WslcProcessSettingsSetEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc)
try
{
    UNREFERENCED_PARAMETER(key_value);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}
CATCH_RETURN();

// PROCESS MANAGEMENT

STDAPI WslcProcessGetPid(_In_ WslcProcess process, _Out_ uint32_t* pid)
try
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(pid);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, exitEvent);

    *exitEvent = nullptr;

    ULONG ulongHandle = 0;

    HRESULT hr = internalType->process->GetExitEvent(&ulongHandle);

    if (SUCCEEDED_LOG(hr))
    {
        *exitEvent = ULongToHandle(ulongHandle);
    }

    return hr;
}
CATCH_RETURN();

// PROCESS RESULT / SIGNALS

STDAPI WslcProcessGetState(_In_ WslcProcess process, _Out_ WslcProcessState* state)
try
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode)
try
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(exitCode);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessSignal(_In_ WslcProcess process, _In_ WslcSignal signal)
try
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(signal);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessSettingsSetIoCallback(
    _In_ WslcProcessSettings* processSettings, _In_ WslcProcessIoHandle ioHandle, _In_opt_ WslcStdIOCallback stdIOCallback, _In_opt_ PVOID context)
try
{
    UNREFERENCED_PARAMETER(processSettings);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(stdIOCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process, _In_ WslcProcessIoHandle ioHandle, _Out_ HANDLE* handle)
try
{
    auto internalType = CheckAndGetInternalType(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, handle);

    *handle = nullptr;

    ULONG ulongHandle = 0;

    HRESULT hr = internalType->process->GetStdHandle(
        static_cast<ULONG>(static_cast<std::underlying_type_t<WslcProcessIoHandle>>(ioHandle)), &ulongHandle);

    if (SUCCEEDED_LOG(hr))
    {
        *handle = ULongToHandle(ulongHandle);
    }

    return hr;
}
CATCH_RETURN();

// IMAGE MANAGEMENT
STDAPI WslcSessionImagePull(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage)
try
{
    auto internalType = CheckAndGetInternalType(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->uri);

    auto progressCallback = ProgressCallback::CreateIf(options);

    // TODO: Auth
    HRESULT hr = internalType->session->PullImage(options->uri, nullptr, progressCallback.get());

    if (FAILED_LOG(hr))
    {
        GetErrorInfoFromCOM(errorMessage);
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcSessionImageImport(_In_ WslcSession session, _In_ const WslcImportImageOptions* options)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionImageLoad(_In_ WslcSession session, _In_ const WslcLoadImageOptions* options)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionImageDelete(_In_ WslcSession session, _In_z_ PCSTR NameOrId)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(NameOrId);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcSessionImageList(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count)
try
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(images);
    UNREFERENCED_PARAMETER(count);
    return E_NOTIMPL;
}
CATCH_RETURN();

// STORAGE

// INSTALL

STDAPI WslcCanRun(_Out_ BOOL* canRun, _Out_ WslcComponentFlags* missingComponents)
try
{
    UNREFERENCED_PARAMETER(canRun);
    UNREFERENCED_PARAMETER(missingComponents);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version)
try
{
    UNREFERENCED_PARAMETER(version);
    return E_NOTIMPL;
}
CATCH_RETURN();

STDAPI WslcInstallWithDependencies(_In_opt_ WslcInstallCallback progressCallback, _In_opt_ PVOID context)
try
{
    UNREFERENCED_PARAMETER(progressCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}
CATCH_RETURN();
