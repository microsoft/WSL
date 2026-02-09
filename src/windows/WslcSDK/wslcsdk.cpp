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


// SESSION DEFINITIONS
STDAPI_(void) WslcSessionInitSettings(_In_ PCWSTR storagePath,
                               _In_ uint32_t cpuCount,
                               _In_ uint64_t memoryMb,
                               _In_ WslcSessionSettings* sessionSettings)
{

    
    // demo test code to show how to cast the opaque struct to the internal struct and set some values. The real implementation would have actual logic here.
    //WSLC_SESSION_OPTIONS_INTERNAL* sessionSettingsinternal = (WSLC_SESSION_OPTIONS_INTERNAL*)sessionSettings;

    UNREFERENCED_PARAMETER(storagePath);
    UNREFERENCED_PARAMETER(cpuCount);
    UNREFERENCED_PARAMETER(memoryMb);
    //UNREFERENCED_PARAMETER(sessionSettings);
    return;
}

STDAPI WslcSessionCreate(_In_ WslcSessionSettings sessionSettings,
                         _Out_ WslcSession* session)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}


STDAPI WslcSessionTerminate(_In_ WslcSession session)
{
    UNREFERENCED_PARAMETER(session);
    return E_NOTIMPL;
}
STDAPI WslcContainerSettingsNetworkingMode(_In_ WslcContainerSettings containerSettings,
                                           _In_ WSLC_ContainerNetworkingMode networkingMode)
{
    UNREFERENCED_PARAMETER(networkingMode);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsDisplayName(_In_ WslcSessionSettings sessionSettings,
                                      _In_ PCWSTR displayName)
{
    UNREFERENCED_PARAMETER(displayName);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsTimeout(_In_ WslcSessionSettings sessionSettings,
                                  uint32_t timeoutMS)
{
    UNREFERENCED_PARAMETER(timeoutMS);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}


STDAPI WslcSessionSettingsVHD(_In_ WslcSessionSettings sessionSettings,
                              _In_ WSLC_VHD_REQUIREMENTS WslcVHDRequirements)
{
    UNREFERENCED_PARAMETER(WslcVHDRequirements);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}
STDAPI WslcContainerSettingsHostName(_In_ WslcContainerSettings containerSettings,
                                     _In_ PCSTR hostName)
{
    UNREFERENCED_PARAMETER(hostName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}


STDAPI WslcContainerSettingsDomainName(_In_ WslcContainerSettings containerSettings,
                                       _In_ PCSTR domainName)
{
    UNREFERENCED_PARAMETER(domainName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}


STDAPI WslcSessionSettingsFlags(_In_ WslcSessionSettings sessionSettings,
                                _In_ const WSLC_SESSION_FLAGS flags)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsTerminateCallback(_In_ WslcSessionSettings sessionSettings,
                                            _In_ WslcSessionTerminationCallback terminationCallback,
                                            _In_ PVOID terminationContext)
{
    UNREFERENCED_PARAMETER(terminationCallback);
    UNREFERENCED_PARAMETER(terminationContext);
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}

STDAPI WslcSessionSettingsRelease(_In_ WslcSessionSettings sessionSettings)
{
    UNREFERENCED_PARAMETER(sessionSettings);
    return E_NOTIMPL;
}
STDAPI WslcSessionRelease(_In_ WslcSession session)
{
    UNREFERENCED_PARAMETER(session);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsRelease(_In_ WslcContainerSettings containerSettings)
{
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
STDAPI WslcContainerRelease(_In_ WslcContainer container)
{
    UNREFERENCED_PARAMETER(container);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsRelease(_In_ WslcProcessSettings processSettings)
{
    UNREFERENCED_PARAMETER(processSettings);
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

STDAPI WslcContainerCreate(_In_ WslcContainerSettings containerSettings,
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

STDAPI WslcContainerSettingsFlags(_In_ WslcContainerSettings containerSettings,
                                  _In_ WSLC_CONTAINER_FLAGS flags)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}
STDAPI WslcContainerSettingsName(_In_ WslcContainerSettings containerSettings,
                                        _In_ PCSTR runtimeName)
{
    UNREFERENCED_PARAMETER(runtimeName);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsInitProcess(_In_ WslcContainerSettings containerSettings,
                                        _In_ WslcProcessSettings initProcess)
{
    UNREFERENCED_PARAMETER(initProcess);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsPortMapping(_In_ WslcContainerSettings containerSettings,
                                        _In_ const WSLC_CONTAINER_PORT_MAPPING* portMappings)
{
    UNREFERENCED_PARAMETER(portMappings);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerSettingsVolume(_In_ WslcContainerSettings containerSettings,
                                   _In_ const WSLC_CONTAINER_VOLUME* volumes)
{
    UNREFERENCED_PARAMETER(volumes);
    UNREFERENCED_PARAMETER(containerSettings);
    return E_NOTIMPL;
}

STDAPI WslcContainerExec(_In_ WslcContainer container,
                                _In_ WslcProcessSettings newProcessSettings,
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
    return E_NOTIMPL;
}

STDAPI WslcContainerDelete(_In_ WslcContainer container,
                           _In_ WslcDeleteContainerFlags flags)
{
    UNREFERENCED_PARAMETER(container);
    return E_NOTIMPL;
}

// PROCESS DEFINITIONS

STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings)
{
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}
STDAPI WslcProcessSettingsEntryPoint(_In_ WslcProcessSettings processSettings,
    _In_ const PCSTR entryPoint)
{
    UNREFERENCED_PARAMETER(entryPoint);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsCurrentDirectory(_In_ WslcProcessSettings processSettings,
                                           _In_ const PCSTR currentDirectory)
{
    UNREFERENCED_PARAMETER(currentDirectory);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsCmdLineArgs(WslcProcessSettings processSettings,
                                      _In_reads_(argc) PCSTR const* argv,
                                       size_t argc)
{
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(processSettings);
    return E_NOTIMPL;
}

STDAPI WslcProcessSettingsEnvVariables(_In_ WslcProcessSettings processSettings,
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
                         _Out_ const UINT32* pid)
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

STDAPI WslcProcessSettingsIoCallback(_In_ WslcProcessSettings processSettings,
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
                            _In_ const WLSC_PULL_IMAGE_OPTIONS* options,
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

STDAPI WslcSessionCreateVhd(_In_ WslcSession session,
                            _In_ const WSLC_VHD_REQUIREMENTS* options)
{
    UNREFERENCED_PARAMETER(session);
    UNREFERENCED_PARAMETER(options);
    return E_NOTIMPL;
}

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
