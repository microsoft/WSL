/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslcsdk.cpp

Abstract:

    This file contains the public WSLC Client SDK api implementations.

--*/
#include "precomp.h"

#include "wslcsdk.h"
#include "wslcsdkprivate.h"
#include "TerminationCallback.h"


namespace
{
    WSLAFeatureFlags ConvertFlags(WSLC_SESSION_FLAGS flags)
    {
        WSLAFeatureFlags result = WslaFeatureFlagsNone;

        // TODO: Many missing flags?
        if (WI_IsFlagSet(flags, WSLC_SESSION_FLAG_ENABLE_GPU))
        {
            result |= WslaFeatureFlagsGPU;
        }

        return result;
    }

    LPCSTR ConvertType(WSLC_VhdType type)
    {
        // TODO: Correct strings? Doesn't appear so, as tracking the code suggests that this is the `filesystemtype` to the linux `mount` function.
        //       Not clear how to map dynamic and fixed to values like `ext4` and `tmpfs`.
        switch (type)
        {
        case WSLC_VhdTypeDynamic:
            return "dynamic";
        case WSLC_VhdTypeFixed:
            return "fixed";
        default:
            return nullptr;
        }
    }
}

// SESSION DEFINITIONS
STDAPI WslcSessionInitSettings(_In_ PCWSTR storagePath,
                                        _In_ uint32_t cpuCount,
                                        _In_ uint64_t memoryMb,
                                        _Out_ WslcSessionSettings* sessionSettings)
{
    // TODO: Do we need to check the path itself for anything?
    // TODO: Ensure memoryMb is not larger than ULONG (unless API change)

    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    *internalType = {};

    internalType->storagePath = storagePath;
    internalType->cpuCount = cpuCount;
    internalType->memoryMb = memoryMb;

    return S_OK;
}

STDAPI WslcSessionCreate(_In_ WslcSessionSettings* sessionSettings,
                         _Out_ WslcSession* session) try
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
    // TODO: memoryMb probably doesn't need to be a 64 bit value, that would be ~2^84 B, or 16 YB. At a very conservatige $1 per GB, this would cost 16 Quadrillion dollars (or ~150 years of current global GDP).
    //       A 32 bit value provides for 4 PB of memory, which is still quite a ways off from being a limiting factor for the API (CoPilot says 40-50 years IFF current scaling can keep going, 15-25 for some "SSDs swaps are integrated into the memory model" theoreticals).
    //       Plus the runtime API only takes a 32 bit value...
    runtimeSettings.MemoryMb = static_cast<ULONG>(internalType->memoryMb);
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
    runtimeSettings.RootVhdOverride = internalType->vhdRequirements.path;
    // TODO: I don't think that this VHD type override can be reused from the VHD requirements type
    runtimeSettings.RootVhdTypeOverride = ConvertType(internalType->vhdRequirements.type);

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

STDAPI WslcContainerSettingsSetNetworkingMode(_In_ WslcContainerSettings* containerSettings,
                                           _In_ WSLC_ContainerNetworkingMode networkingMode)
{
    UNREFERENCED_PARAMETER(networkingMode);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsSetDisplayName(_In_ WslcSessionSettings* sessionSettings,
                                         _In_ PCWSTR displayName)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->displayName = displayName;

    return S_OK;
}

STDAPI WslcSessionSettingsSetTimeout(_In_ WslcSessionSettings* sessionSettings,
                                     uint32_t timeoutMS)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->timeoutMS = timeoutMS;

    return S_OK;
}

STDAPI WslcSessionCreateVhd(_In_ WslcSession sesssion,
                            _In_ const WSLC_VHD_REQUIREMENTS* options)
{
    UNREFERENCED_PARAMETER(sesssion);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsSetVHD(_In_ WslcSessionSettings* sessionSettings,
                              _In_ WSLC_VHD_REQUIREMENTS* vhdRequirements)
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
STDAPI WslcContainerSettingsSetHostName(_In_ WslcContainerSettings* containerSettings,
                                     _In_ const PCSTR hostName)
{
    UNREFERENCED_PARAMETER(hostName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}


STDAPI WslcContainerSettingsSetDomainName(_In_ WslcContainerSettings* containerSettings,
                                       _In_ const PCSTR domainName)
{
    UNREFERENCED_PARAMETER(domainName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}


STDAPI WslcSessionSettingsSetFlags(_In_ WslcSessionSettings* sessionSettings,
                                _In_ const WSLC_SESSION_FLAGS flags)
{
    WSLC_GET_INTERNAL_TYPE(sessionSettings);

    internalType->flags = flags;

    return S_OK;
}

STDAPI WslcSessionSettingsSetTerminateCallback(_In_ WslcSessionSettings* sessionSettings,
                                            _In_ WslcSessionTerminationCallback terminationCallback,
                                            _In_ PVOID terminationContext)
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

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName,
                                 _Out_ WslcContainerSettings* containerSettings)
{
    UNREFERENCED_PARAMETER(imageName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerCreate(_In_ WslcContainerSettings* containerSettings,
                           _Out_ WslcContainer* container, 
                           _Outptr_opt_result_z_ PWSTR* errorMessage)
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

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings,
                                  _In_ WSLC_CONTAINER_FLAGS flags)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings,
                                        _In_ PCSTR name)
{
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings,
                                           _In_ WslcProcessSettings* initProcess)
{
    UNREFERENCED_PARAMETER(initProcess);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetPortMapping(_In_ WslcContainerSettings* containerSettings,
                                        _In_ const WSLC_CONTAINER_PORT_MAPPING* portMappings)
{
    UNREFERENCED_PARAMETER(portMappings);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsSetVolume(_In_ WslcContainerSettings* containerSettings,
                                      _In_ const WSLC_CONTAINER_VOLUME* volumes)
{
    UNREFERENCED_PARAMETER(volumes);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerExec(_In_ WslcContainer container,
                                _In_ WslcProcessSettings *newProcessSettings,
                                _Out_ WslcProcess* newProcess)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(newProcessSettings); 
    UNREFERENCED_PARAMETER(newProcess);
    return E_NOTIMPL;
}

// GENERAL CONTAINER MANAGEMENT

STDAPI WslcContainerGetID(WslcContainer container,
                          PCHAR(*containerId)[WSLC_CONTAINER_ID_LENGTH])
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(containerId);
    return E_NOTIMPL;
}

STDAPI WslcContainerInspect(_In_ WslcContainer container,
                            _Outptr_result_z_ PCSTR* inspectData)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(inspectData);
    return E_NOTIMPL;
}

STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container,
                                   _Out_ WslcProcess* initProcess)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(initProcess);
    return E_NOTIMPL;
}


STDAPI WslcContainerGetState(_In_ WslcContainer container,
                             _Out_ WSLC_CONTAINER_STATE* state)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}



STDAPI WslcContainerStop(_In_ WslcContainer container,
                         _In_ WSLC_PROCESS_SIGNAL signal,
                         _In_ uint32_t timeoutMS)
{
    UNREFERENCED_PARAMETER(container);
    UNREFERENCED_PARAMETER(signal);
    UNREFERENCED_PARAMETER(timeoutMS);
    return E_NOTIMPL;
}

STDAPI WslcContainerDelete(_In_ WslcContainer container,
                           _In_ WslcDeleteContainerFlags flags)
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
STDAPI WslcProcessSettingsSetExecutable(_In_ WslcProcessSettings* processSettings,
    _In_ const PCSTR executable)
{
    UNREFERENCED_PARAMETER(executable);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsSetCurrentDirectory(_In_ WslcProcessSettings* processSettings,
                                           _In_ const PCSTR currentDirectory)
{
    UNREFERENCED_PARAMETER(currentDirectory);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsSetCmdLineArgs(WslcProcessSettings* processSettings,
                                      _In_reads_(argc) PCSTR const* argv, 
                                      size_t argc)
{
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsSetEnvVariables(_In_ WslcProcessSettings* processSettings,
                                       _In_reads_(argc) PCSTR const* key_value,
                                       size_t argc)
{
    UNREFERENCED_PARAMETER(key_value);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

// PROCESS MANAGEMENT

STDAPI WslcProcessGetPid(_In_ WslcProcess process,
                         _Out_ UINT32* pid)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(pid);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process,
                               _Out_ HANDLE* exitEvent)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(exitEvent);
    return E_NOTIMPL;
}
EXTERN_C_START

// PROCESS RESULT / SIGNALS


STDAPI WslcProcessGetState(_In_ WslcProcess process,
                           _Out_ WSLC_PROCESS_STATE* state)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(state);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process,
                              _Out_ PINT32 exitCode)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(exitCode);
    return E_NOTIMPL;
}

STDAPI WslcProcessSignal(_In_ WslcProcess process,
                         _In_ WSLC_PROCESS_SIGNAL signal)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(signal);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsSetIoCallback(_In_ WslcProcessSettings* processSettings,
                                     _In_ WSLC_PROCESS_IO_HANDLE ioHandle,
                                     _In_ WslcStdIOCallback stdIOCallback,
                                     _In_opt_ PVOID context)
{
    UNREFERENCED_PARAMETER(processSettings);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(stdIOCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}

STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process,
                               _In_ WSLC_PROCESS_IO_HANDLE ioHandle,
                               _Out_ HANDLE* handle)
{
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(handle);
    return E_NOTIMPL;
}

// IMAGE MANAGEMENT
STDAPI WslcSessionImagePull(_In_ WslcSession session,
                            _In_ const WSLC_PULL_IMAGE_OPTIONS* options,
                            _Outptr_opt_result_z_ PWSTR* errorMessage)
{
    UNREFERENCED_PARAMETER(options);
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(errorMessage);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageImport(_In_ WslcSession session,
                              _In_ const WSLC_IMPORT_CONTAINER_IMAGE_OPTIONS* options)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageLoad(_In_ WslcSession session,
                            _In_ const WSLC_LOAD_CONTAINER_IMAGE_OPTIONS* options)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageDelete(_In_ WslcSession session,
                              _In_z_ PCSTR NameOrId)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(NameOrId);
    return E_NOTIMPL;
}

STDAPI WslcSessionImageList(_In_ WslcSession session,
                            _Outptr_result_buffer_(*count) WSLC_IMAGE_INFO** images, _Out_ UINT32* count)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(images);
    UNREFERENCED_PARAMETER(count);
    return E_NOTIMPL;
}

// STORAGE



// INSTALL

STDAPI WslcCanRun(_Out_ BOOL* canRun,
                  _Out_ WSLC_COMPONENT_FLAGS* missingComponents)
{
    UNREFERENCED_PARAMETER(canRun);
    UNREFERENCED_PARAMETER(missingComponents);
    return E_NOTIMPL;
}

STDAPI WslcGetVersion(_Out_writes_(1) WSLC_VERSION* version)
{
    UNREFERENCED_PARAMETER(version);
    return E_NOTIMPL;
}

STDAPI WslcInstallWithDependencies(_In_opt_ __callback WslcInstallCallback progressCallback,
                                   _In_opt_ PVOID context)
{
    UNREFERENCED_PARAMETER(progressCallback);
    UNREFERENCED_PARAMETER(context);
    return E_NOTIMPL;
}


EXTERN_C_END