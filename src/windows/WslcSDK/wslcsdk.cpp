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


namespace
{
    constexpr uint32_t s_DefaultCPUCount = 2;
    constexpr uint32_t s_DefaultMemoryMB = 2000;

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
    // runtimeSettings.MaximumStorageSizeMb;
    runtimeSettings.CpuCount = internalType->cpuCount;
    runtimeSettings.MemoryMb = internalType->memoryMb;
    runtimeSettings.BootTimeoutMs = internalType->timeoutMS;
    // TODO: No user control over networking mode (NAT and VirtIO)?
    runtimeSettings.NetworkingMode = WSLANetworkingModeNone;
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

    internalType->timeoutMS = timeoutMS;

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
    UNREFERENCED_PARAMETER(container);
    return E_NOTIMPL;
}
STDAPI WslcProcessRelease(_In_ WslcProcess process)
{
    UNREFERENCED_PARAMETER(process);
    return E_NOTIMPL;
}

// CONTAINER DEFINITIONS

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings)
{
    UNREFERENCED_PARAMETER(imageName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerCreate(_In_ WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage)
{
    UNREFERENCED_PARAMETER(containerSettings);
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(errorMessage);
    return E_NOTIMPL;
}

STDAPI WslcContainerStart(_In_ WslcContainer container)
{
    UNREFERENCED_PARAMETER(container);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name)
{
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess)
{
    UNREFERENCED_PARAMETER(initProcess);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
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
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(initProcess);
    return E_NOTIMPL;
}

STDAPI WslcContainerGetState(_In_ WslcContainer container, _Out_ WslcContainerState* state)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}

STDAPI WslcContainerStop(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutMS)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(signal);
    UNREFERENCED_PARAMETER(timeoutMS);
    return E_NOTIMPL;
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
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}
STDAPI WslcProcessSettingsSetExecutable(_In_ WslcProcessSettings* processSettings, _In_ const PCSTR executable)
{
    UNREFERENCED_PARAMETER(executable);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
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
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
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
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(handle);
    return E_NOTIMPL;
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
    RETURN_IF_FAILED(internalType->session->PullImage(options->uri, nullptr, progressCallback.get()));

    GetErrorInfoIf(errorMessage);

    return S_OK;
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
