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


// TODO: Larger TODO bucket
//          - DisplayName is more like SessionKey and must be provided
//          - Networking mode has cannot be provided and is critical to pulling images
//          - Process options separates executable and cmdline, but there is no separation in the runtime API

namespace
{
    constexpr uint32_t s_DefaultCPUCount = 2;
    constexpr uint32_t s_DefaultMemoryMB = 2000;
    // Maximum value per use with HVSOCKET_CONNECT_TIMEOUT_MAX
    constexpr ULONG s_DefaultBootTimeout = 300000;
    // Default to 1 GB
    constexpr UINT64 s_DefaultStorageSize = 1000 * 1000 * 1000;

    WSLAFeatureFlags ConvertFlags(WslcSessionFlags flags)
    {
        WSLAFeatureFlags result = WslaFeatureFlagsNone;

        // TODO: Many missing flags?
        if (WI_IsFlagSet(flags, WSLC_SESSION_FLAG_ENABLE_GPU))
        {
            result |= WslaFeatureFlagsGPU;
        }

        return result;
    }

    std::optional<WSLASignal> ConvertSignal(WslcSignal signal)
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
            return std::nullopt;
        }
    }

    void GetErrorInfoIf(PWSTR* errorMessage)
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
}

// SESSION DEFINITIONS
STDAPI WslcSessionInitSettings(_In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings)
{
    // TODO: Do we need to check the path itself for anything?

    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    *internalType = {};

    internalType->storagePath = storagePath;
    internalType->cpuCount = s_DefaultCPUCount;
    internalType->memoryMb = s_DefaultMemoryMB;
    internalType->timeoutMS = s_DefaultBootTimeout;
    internalType->vhdRequirements.sizeInBytes = s_DefaultStorageSize;

    return S_OK;
}

STDAPI WslcSessionSettingsSetCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    if (cpuCount)
    {
        internalType->cpuCount = cpuCount;
    }
    else
    {
        // TODO: Does a 0 cause the internal systems to use a default value?
        // From reading the code it appears to just send 0 to HcsCreateComputeSystem, but the documentation is not clear on what that will do.
        internalType->cpuCount = s_DefaultCPUCount;
    }

    return S_OK;
}

STDAPI WslcSessionSettingsSetMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMb)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    if (memoryMb)
    {
        internalType->memoryMb = memoryMb;
    }
    else
    {
        // TODO: Does a 0 cause the internal systems to use a default value?
        // From reading the code it appears to just send 0 to HcsCreateComputeSystem, but the documentation is not clear on what that will do.
        internalType->memoryMb = s_DefaultMemoryMB;
    }

    return S_OK;
}

STDAPI WslcSessionCreate(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session) try
{
    RETURN_HR_IF_NULL(E_POINTER, session);
    *session = nullptr;

    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    wil::com_ptr<IWSLASessionManager> sessionManager;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(WSLASessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
    wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

    auto result = std::make_unique<WslcSessionImpl>();
    WSLA_SESSION_SETTINGS runtimeSettings{};
    runtimeSettings.DisplayName = internalType->displayName;
    runtimeSettings.StoragePath = internalType->storagePath;
    // TODO: Is this VHD requirements sizeInBytes?
    runtimeSettings.MaximumStorageSizeMb = internalType->vhdRequirements.sizeInBytes / (1000 * 1000);
    runtimeSettings.CpuCount = internalType->cpuCount;
    runtimeSettings.MemoryMb = internalType->memoryMb;
    runtimeSettings.BootTimeoutMs = internalType->timeoutMS;
    // TODO: No user control over networking mode (NAT and VirtIO)?
    runtimeSettings.NetworkingMode = WSLANetworkingModeVirtioProxy;
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
CATCH_RETURN()

STDAPI WslcSessionTerminate(_In_ WslcSession session)
{
    WSLC_GET_INTERNAL_TYPE(session);

    if (internalType->session)
    {
        return internalType->session->Terminate();
    }

    // TODO: Should we fail if session invalid?
    return S_FALSE;
}

STDAPI WslcContainerSettingsSetNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode)
{
    UNREFERENCED_PARAMETER(networkingMode);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsSetDisplayName(_In_ WslcSessionSettings* sessionSettings, _In_ PCWSTR displayName)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->displayName = displayName;

    return S_OK;
}

STDAPI WslcSessionSettingsSetTimeout(_In_ WslcSessionSettings* sessionSettings, uint32_t timeoutMS)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    if (timeoutMS)
    {
        internalType->timeoutMS = timeoutMS;
    }
    else
    {
        // TODO: 0 is not treated as no timeout within the runtime, it is an immediate timeout.
        internalType->timeoutMS = s_DefaultBootTimeout;
    }

    return S_OK;
}

STDAPI WslcSessionCreateVhd(_In_ WslcSession sesssion, _In_ const WslcVhdRequirements* options)
{
    UNREFERENCED_PARAMETER(sesssion);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsSetVHD(_In_ WslcSessionSettings* sessionSettings, _In_ const WslcVhdRequirements* vhdRequirements)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

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

STDAPI WslcContainerSettingsSetHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName)
{
    UNREFERENCED_PARAMETER(hostName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName)
{
    UNREFERENCED_PARAMETER(domainName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsSetFlags(_In_ WslcSessionSettings* sessionSettings, _In_ const WslcSessionFlags flags)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->flags = flags;

    return S_OK;
}

STDAPI WslcSessionSettingsSetTerminateCallback(
    _In_ WslcSessionSettings* sessionSettings, _In_opt_ WslcSessionTerminationCallback terminationCallback, _In_opt_ PVOID terminationContext)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->terminationCallback = terminationCallback;
    internalType->terminationCallbackContext = terminationContext;

    return S_OK;
}

STDAPI WslcSessionRelease(_In_ WslcSession session)
{
    WSLC_GET_INTERNAL_TYPE_FOR_RELEASE(session);

    // Intentionally destroy session before termination callback in the event that
    // the termination callback ends up being invoked by session destruction.
    internalType->session.reset();
    internalType->terminationCallback.reset();

    return S_OK;
}

STDAPI WslcContainerRelease(_In_ WslcContainer container)
{
    WSLC_GET_INTERNAL_TYPE_FOR_RELEASE(container);

    return S_OK;
}

STDAPI WslcProcessRelease(_In_ WslcProcess process)
{
    WSLC_GET_INTERNAL_TYPE_FOR_RELEASE(process);

    return S_OK;
}

// CONTAINER DEFINITIONS

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings)
{
    WSLC_GET_INTERNAL_TYPE(containerSettings);
    RETURN_HR_IF_NULL(E_POINTER, imageName);

    *internalType = {};

    internalType->image = imageName;

    return S_OK;
}

STDAPI WslcContainerCreate(_In_ WslcSession session, _In_ WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage) try
{
    RETURN_HR_IF_NULL(E_POINTER, container);
    *container = nullptr;

    WSLC_GET_INTERNAL_TYPE_NAMED(session, internalSession);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalSession->session);
    WSLC_GET_INTERNAL_TYPE_NAMED(containerSettings, internalContainerSettings);

    auto result = std::make_unique<WslcContainerImpl>();

    WSLA_CONTAINER_OPTIONS containerOptions{};
    containerOptions.Image = internalContainerSettings->image;
    containerOptions.Name = internalContainerSettings->runtimeName;
    containerOptions.HostName = internalContainerSettings->HostName;
    containerOptions.DomainName = internalContainerSettings->DomainName;

    const WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* initProcessOptions = internalContainerSettings->initProcessOptions;
    if (initProcessOptions)
    {
        containerOptions.InitProcessOptions.CurrentDirectory = initProcessOptions->currentDirectory;
        // TODO:Runtime needs update
        containerOptions.InitProcessOptions.CommandLine.Values = const_cast<LPCSTR*>(initProcessOptions->commandLine);
        containerOptions.InitProcessOptions.CommandLine.Count = initProcessOptions->commandLineCount;
        containerOptions.InitProcessOptions.Environment.Values = const_cast<LPCSTR*>(initProcessOptions->environment);
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
    // containerOptions.Flags;
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
        GetErrorInfoIf(errorMessage);
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
{
    WSLC_GET_INTERNAL_TYPE(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    // TODO: No user choice between None and Attach (where attach is what allows access to init process IO handles)
    RETURN_IF_FAILED(internalType->container->Start(WSLAContainerStartFlagsAttach));

    return S_OK;
}

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name)
{
    WSLC_GET_INTERNAL_TYPE(containerSettings);

    // TODO: Is this the correct name? How does one set the DomainName and HostName?
    internalType->runtimeName = name;

    return S_OK;
}

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess)
{
    WSLC_GET_INTERNAL_TYPE(containerSettings);

    internalType->initProcessOptions = GetInternalType(initProcess);

    return S_OK;
}

STDAPI WslcContainerSettingsSetPortMapping(_In_ WslcContainerSettings* containerSettings, _In_ const WslcContainerPortMapping* portMappings, _In_ UINT32 portMappingCount)
{
    UNREFERENCED_PARAMETER(portMappings);
    UNREFERENCED_PARAMETER(containerSettings);
    UNREFERENCED_PARAMETER(portMappingCount);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsAddVolume(_In_ WslcContainerSettings* containerSettings, _In_ const WslcContainerVolume* volumes, _In_ UINT32 volumeCount)
{
    UNREFERENCED_PARAMETER(volumes);
    UNREFERENCED_PARAMETER(volumeCount);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerExec(_In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(newProcessSettings);
    UNREFERENCED_PARAMETER(newProcess);
    return E_NOTIMPL;
}

// GENERAL CONTAINER MANAGEMENT

STDAPI WslcContainerGetID(WslcContainer container, PCHAR (*containerId)[WSLC_CONTAINER_ID_LENGTH])
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(containerId);
    return E_NOTIMPL;
}

STDAPI WslcContainerInspect(_In_ WslcContainer container, _Outptr_result_z_ PCSTR* inspectData)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(inspectData);
    return E_NOTIMPL;
}

STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess)
{
    WSLC_GET_INTERNAL_TYPE(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);
    RETURN_HR_IF_NULL(E_POINTER, initProcess);

    *initProcess = nullptr;

    auto result = std::make_unique<WslcProcessImpl>();

    RETURN_IF_FAILED(internalType->container->GetInitProcess(&result->process));

    *initProcess = reinterpret_cast<WslcProcess>(result.release());

    return S_OK;
}

STDAPI WslcContainerGetState(_In_ WslcContainer container, _Out_ WslcContainerState* state)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}

STDAPI WslcContainerStop(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutMS)
{
    WSLC_GET_INTERNAL_TYPE(container);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->container);

    auto convertedSignal = ConvertSignal(signal);
    RETURN_HR_IF(E_INVALIDARG, !convertedSignal);

    // TODO: Resolve massive disparity between 32-bit unsigned millisecond input and 64-bit signed second target timeouts
    RETURN_IF_FAILED(internalType->container->Stop(convertedSignal.value(), timeoutMS / 1000));

    return S_OK;
}

STDAPI WslcContainerDelete(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(flags);
    return E_NOTIMPL;
}

// PROCESS DEFINITIONS

STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings)
{
    WSLC_GET_INTERNAL_TYPE(processSettings);

    *internalType = {};

    return S_OK;
}
STDAPI WslcProcessSettingsSetExecutable(_In_ WslcProcessSettings* processSettings, _In_ const PCSTR executable)
{
    WSLC_GET_INTERNAL_TYPE(processSettings);

    internalType->executable = executable;

    return S_OK;
}

STDAPI WslcProcessSettingsSetCurrentDirectory(_In_ WslcProcessSettings* processSettings, _In_ const PCSTR currentDirectory)
{
    UNREFERENCED_PARAMETER(currentDirectory);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsSetCmdLineArgs(WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc)
{
    WSLC_GET_INTERNAL_TYPE(processSettings);
    RETURN_HR_IF(
        E_INVALIDARG,
        (argv == nullptr && argc != 0) || (argv != nullptr && argc == 0) ||
            (argc > static_cast<size_t>(std::numeric_limits<UINT32>::max())));

    internalType->commandLine = argv;
    internalType->commandLineCount = static_cast<UINT32>(argc);

    return S_OK;
}

STDAPI WslcProcessSettingsSetEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc)
{
    UNREFERENCED_PARAMETER(key_value);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

// PROCESS MANAGEMENT

STDAPI WslcProcessGetPid(_In_ WslcProcess process, _Out_ UINT32* pid)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(pid);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(exitEvent);
    return E_NOTIMPL;
}

// PROCESS RESULT / SIGNALS

STDAPI WslcProcessGetState(_In_ WslcProcess process, _Out_ WslcProcessState* state)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(exitCode);
    return E_NOTIMPL;
}

STDAPI WslcProcessSignal(_In_ WslcProcess process, _In_ WslcSignal signal)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(signal);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsSetIoCallback(
    _In_ WslcProcessSettings* processSettings, _In_ WslcProcessIoHandle ioHandle, _In_ WslcStdIOCallback stdIOCallback, _In_opt_ PVOID context)
{
    UNREFERENCED_PARAMETER(processSettings);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(stdIOCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process, _In_ WslcProcessIoHandle ioHandle, _Out_ HANDLE* handle)
{
    WSLC_GET_INTERNAL_TYPE(process);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->process);
    RETURN_HR_IF_NULL(E_POINTER, handle);

    // TODO: INVALID_HANDLE?
    *handle = nullptr;

    ULONG ulongHandle = 0;

    HRESULT hr = internalType->process->GetStdHandle(static_cast<ULONG>(static_cast<std::underlying_type_t<WslcProcessIoHandle>>(ioHandle)), &ulongHandle);

    if (SUCCEEDED_LOG(hr))
    {
        *handle = ULongToHandle(ulongHandle);
    }

    return hr;
}

// IMAGE MANAGEMENT
STDAPI WslcSessionImagePull(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage) try
{
    WSLC_GET_INTERNAL_TYPE(session);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), internalType->session);
    RETURN_HR_IF_NULL(E_POINTER, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, options->uri);

    auto progressCallback = ProgressCallback::CreateIf(options);

    // TODO: Auth
    HRESULT hr = internalType->session->PullImage(options->uri, nullptr, progressCallback.get());

    if (FAILED_LOG(hr))
    {
        GetErrorInfoIf(errorMessage);
    }

    return hr;
}
CATCH_RETURN();

STDAPI WslcSessionImageImport(_In_ WslcSession session, _In_ const WslcImportImageOptions* options)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageLoad(_In_ WslcSession session, _In_ const WslcLoadImageOptions* options)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageDelete(_In_ WslcSession session, _In_z_ PCSTR NameOrId)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(NameOrId);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageList(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ UINT32* count)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(images);
    UNREFERENCED_PARAMETER(count);
    return E_NOTIMPL;
}

// STORAGE

// INSTALL

STDAPI WslcCanRun(_Out_ BOOL* canRun, _Out_ WslcComponentFlags* missingComponents)
{
    UNREFERENCED_PARAMETER(canRun);
    UNREFERENCED_PARAMETER(missingComponents);
    return E_NOTIMPL;
}

STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version)
{
    UNREFERENCED_PARAMETER(version);
    return E_NOTIMPL;
}

STDAPI WslcInstallWithDependencies(_In_opt_ __callback WslcInstallCallback progressCallback, _In_opt_ PVOID context)
{
    UNREFERENCED_PARAMETER(progressCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}
