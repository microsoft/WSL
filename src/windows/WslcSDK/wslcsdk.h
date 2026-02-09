/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDK.h

Abstract:

    This file contains the public WSL Container SDK api definitions.

--*/
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>


EXTERN_C_START

//Session values
#define WSLC_SESSION_OPTIONS_SIZE 80
#define WSLC_SESSION_OPTIONS_ALIGNMENT 8

typedef struct WslcSessionSettings
{
    __declspec(align(WSLC_SESSION_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_SESSION_OPTIONS_SIZE];
} WslcSessionSettings;


DECLARE_HANDLE(WslcSession);

//Container values
#define WSLC_CONTAINER_OPTIONS_SIZE 80
#define WSLC_CONTAINER_OPTIONS_ALIGNMENT 8

typedef struct WslcContainerSettings
{
    __declspec(align(WSLC_CONTAINER_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_OPTIONS_SIZE];
} WslcContainerSettings;

DECLARE_HANDLE(WslcContainer);

//Process values
#define WSLC_CONTAINER_PROCESS_OPTIONS_SIZE 48
#define WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT 8
typedef struct WslcProcessSettings
{
    __declspec(align(WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_PROCESS_OPTIONS_SIZE];
} WslcProcessSettings;

DECLARE_HANDLE(WslcProcess);


typedef enum WSLC_ContainerNetworkingMode
{
    WSLA_NetworkingModeNone = 0,   // No networking / isolated
    WSLA_NetworkingModeBridged,    
} WSLC_ContainerNetworkingMode;

typedef enum WSLC_VhdType
{
    WSLA_VhdTypeDynamic = 0, // Expanding VHDX (default)
    WSLA_VhdTypeFixed,       // Fully allocated VHDX
} WSLC_VhdType;

typedef struct WSLC_VHD_REQUIREMENTS
{
    _In_z_ PCWSTR path;      // Full path to the VHD/VHDX file
    _In_ UINT64 sizeInBytes;    // Desired size (for create/expand)
    _In_ WSLC_VhdType type;     // Dynamic / Fixed
} WSLC_VHD_REQUIREMENTS;

typedef enum WSLC_SESSION_FLAGS
{
    WSLC_SESSION_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FLAG_ENABLE_GPU = 0x00000001
} WSLC_SESSION_FLAGS;

typedef enum WSLC_SESSION_TERMINATION_REASON
{
    WSLC_SESSION_TERMINATION_REASON_UNKNOWN = 0,
    WSLC_SESSION_TERMINATION_REASON_SHUTDOWN = 1,
    WSLC_SESSION_TERMINATION_REASON_CRASHED = 2,
} WSLC_SESSION_TERMINATION_REASON;

typedef __callback VOID(CALLBACK WslcSessionTerminationCallback)(_In_ WSLC_SESSION_TERMINATION_REASON reason, _In_opt_ PVOID context);

STDAPI WslcSessionInitSettings(_In_ PCWSTR storagePath,
                               _In_ uint32_t cpuCount,
                               _In_ uint64_t memoryMb,
                               _Out_ WslcSessionSettings* sessionSettings);

STDAPI WslcSessionCreate(_In_ WslcSessionSettings sessionSettings,
                         _Out_ WslcSession* session);
   
//OPTIONAL SESSION SETTINGS

STDAPI WslcSessionSettingsVHD(_Inout_ WslcSessionSettings* sessionSettings,
                                 _In_ WSLC_VHD_REQUIREMENTS vhdRequirements);

STDAPI WslcSessionSettingsTimeout(_Inout_ WslcSessionSettings* sessionSettings,
                                  uint32_t timeoutMS);


STDAPI WslcSessionSettingsFlags(_Inout_ WslcSessionSettings* sessionSettings,
                                   _In_ WSLC_SESSION_FLAGS flags);

// Pass in Null for callback to clear the termination callback
STDAPI WslcSessionSettingsTerminateCallback(_Inout_ WslcSessionSettings* sessionSettings,
                                            _In_opt_ WslcSessionTerminationCallback terminationCallback,
                                            _In_opt_ PVOID terminationContext);


STDAPI WslcSessionTerminate(_In_ WslcSession session);
STDAPI WslcSessionSettingsRelease(_In_ WslcSessionSettings sessionSettigs);
STDAPI WslcSessionRelease(_In_ WslcSession session);



//CONTAINER DEFINITIONS

typedef enum WSLC_PORT_PROTOCOL
{
    WslcProtocol_TCP = 0,
    WslcProtocol_UDP = 1
} WSLC_PORT_PROTOCOL;

typedef struct WSLC_CONTAINER_PORT_MAPPING
{
    _In_ UINT16 windowsPort;          // Port on Windows host
    _In_ UINT16 containerPort;        // Port inside container
    _In_ WSLC_PORT_PROTOCOL protocol; // TCP or UDP

    // Optional override for the default binding address. If NULL, the default
    // address will be used. The buffer must point to a valid sockaddr (IPv4 or
    // IPv6) of length windowsAddressLength bytes.
    _In_reads_bytes_opt_(windowsAddressLength) const struct sockaddr* windowsAddress;
    _In_ INT windowsAddressLength;
}WSLC_CONTAINER_PORT_MAPPING;

typedef struct WSLC_CONTAINER_VOLUME
{
    _In_z_ PCWSTR windowsPath;
    _In_z_ PCSTR containerPath;
    _In_ BOOL readOnly;
} WSLC_CONTAINER_VOLUME;


typedef enum WSLC_CONTAINER_FLAGS
{
    WSLC_CONTAINER_FLAG_NONE = 0x00000000,
    WSLC_CONTAINER_FLAG_ENABLE_GPU = 0x00000001,
    WSLC_CONTAINER_FLAG_PRIVILEGED = 0x00000002,
    WSLC_CONTAINER_FLAG_AUTO_REMOVE = 0x00000004,

} WSLC_CONTAINER_FLAGS;

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName,
                                 _Out_ WslcContainerSettings* containerSettings);

STDAPI WslcContainerCreate(_In_ WslcContainerSettings containerSettings,
                           _Out_ WslcContainer* container,
                           _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcContainerStart(_In_ WslcContainer container);

//OPTIONAL CONTAINER SETTINGS
STDAPI WslcContainerSettingsRuntimeName(_In_ WslcContainerSettings containerSettings,
                                        _In_ PCSTR runtimeName);

STDAPI WslcContainerSettingsInitProcess(_In_ WslcContainerSettings containerSettings,
                                        _In_ WslcProcessSettings initProcess);

STDAPI WslcContainerSettingsNetworkingMode(_In_ WslcContainerSettings containerSettings,
                                           _In_ WSLC_ContainerNetworkingMode networkingMode);
                                           

STDAPI WslcContainerSettingsHostName(_In_ WslcContainerSettings containerSettings,
                                     _In_ const PCSTR hostName);

STDAPI WslcContainerSettingsDomainName(_In_ WslcContainerSettings containerSettings,
                                       _In_ const PCSTR domainName);


STDAPI WslcContainerSettingsFlags(_In_ WslcContainerSettings containerSettings,
                                  _In_ WSLC_CONTAINER_FLAGS flags);

STDAPI WslcContainerSettingsPortMapping(_In_ WslcContainerSettings containerSettings,
                                        _In_ const WSLC_CONTAINER_PORT_MAPPING* portMappings);

                              // Add the container volume to the volumes array
STDAPI WslcContainerSettingsVolume(_In_ WslcContainerSettings containerSettings, _In_ const WSLC_CONTAINER_VOLUME* volumes);



STDAPI WslcContainerExecProcess(_In_ WslcContainer container,
                                _In_ WslcProcessSettings newProcessSettings,
                                _Out_ WslcProcess* newProcess);

STDAPI WslcContainerSettingsRelease(_In_ WslcContainerSettings containerSettings);
STDAPI WslcContainerRelease(_In_ WslcContainer container);



//GENERAL CONTAINER MANAGEMENT

#define WSLC_CONTAINER_ID_LENGTH 65 // 64 chars + null

STDAPI WslcContainerGetID(WslcContainer container,
                          PCHAR (*containerId)[WSLC_CONTAINER_ID_LENGTH]);




STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container,
                                   _Out_ WslcProcess* initProcess);
                            
// Retrieves the inspection data for a container.
//
// Parameters:
//   container
//       A valid WslcContainer handle representing the container to inspect.
//
//   inspectData
//       On success, receives a pointer to a null-terminated ANSI string
//       containing the inspection data.
//
//       The string is allocated using CoTaskMemAlloc. The caller takes
//       ownership of the returned memory and must free it by calling
//       CoTaskMemFree when it is no longer needed.
//
// Return Value:
//   S_OK on success. Otherwise, an HRESULT error code indicating the failure.
//
// Notes:
//   - The caller must pass a non-null pointer to a PCSTR variable.
//   - The returned string is immutable and must not be modified by the caller.
STDAPI WslcContainerInspect(_In_ WslcContainer container, _Outptr_result_z_ PCSTR* inspectData);


typedef enum WSLC_CONTAINER_STATE
{
    WSLC_CONTAINER_STATE_INVALID = 0,
    WSLC_CONTAINER_STATE_CREATED = 1,
    WSLC_CONTAINER_STATE_RUNNING = 2,
    WSLC_CONTAINER_STATE_EXITED = 3,
    WSLC_CONTAINER_STATE_FAILED = 4,
} WSLC_CONTAINER_STATE;

STDAPI WslcContainerGetState(_In_ WslcContainer container,
                             _Out_ WSLC_CONTAINER_STATE* state);

STDAPI WslcContainerStop(_In_ WslcContainer container);

STDAPI WslcContainerDelete(_In_ WslcContainer container);

//PROCESS DEFINITIONS
STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings);

//OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsEntryPoint(_In_ WslcProcessSettings processSettings,
                                     _In_ const PCSTR entryPoint);

STDAPI WslcProcessSettingsCurrentDirectory(_In_ WslcProcessSettings processSettings,
                                           _In_ const PCSTR currentDirectory);


STDAPI WslcProcessSettingsCmdLineArgs(WslcProcessSettings processSettings,
                                      PWCHAR const* argv,
                                      size_t argc);


STDAPI WslcProcessSettingsEnvVariables(_In_ WslcProcessSettings processSettings,
                                       _In_ PWCHAR const* key_value,
                                       size_t argc);

// Callback invoked when stdout or stderr data is available from a running
// WSLC process.
//
// Parameters:
//   data
//       Pointer to a buffer containing the bytes read. The buffer is owned
//       by WSLC and is valid only for the duration of the callback.
//
//       The caller must not free, modify, or retain the pointer. If the
//       caller needs to keep the data, it must copy the contents before
//       returning from the callback.
//
//   dataSize
//       Number of bytes available in the data buffer.
//
//   context
//       Caller-supplied context pointer that was provided when the callback
//       was registered.
//
// Notes:
//   - WSLC frees or reuses the buffer immediately after the callback returns.
//   - The callback must return promptly; long-running operations may block
//     WSLC's internal I/O processing.
//   - The buffer is not null-terminated; it is a raw byte sequence.
//
typedef __callback VOID(CALLBACK WslcStdIOCallback)(_In_reads_bytes_(dataSize) const BYTE* data, _In_ UINT32 dataSize, _In_opt_ PVOID context);
typedef enum WSLC_PROCESS_IO_HANDLE
{
    stdIn = 0,
    stdOut = 1,
    stdErr = 2
} WSLC_PROCESS_IO_HANDLE;

// Pass in Null for stdIOCallback to clear the callback for the given handle
STDAPI WslcProcessSettingsIoCallback(_In_ WslcProcessSettings processSettings,
                                     _In_ WSLC_PROCESS_IO_HANDLE ioHandle,
                                     _In_opt_ WslcStdIOCallback stdIOCallback);

//PROCESS MANAGEMENT

STDAPI WslcProcessGetPid(_In_ WslcProcess process,
                         _Out_ UINT32* pid);

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process,
                               _Out_ HANDLE* exitEvent);

typedef enum WSLC_PROCESS_STATE
{
    WSLC_CONTAINER_PROCESS_STATE_UNKNOWN = 0,
    WSLC_CONTAINER_PROCESS_STATE_CREATED = 1,
    WSLC_CONTAINER_PROCESS_STATE_RUNNING = 2,
    WSLC_CONTAINER_PROCESS_STATE_EXITED = 3,
} WSLC_PROCESS_STATE;
STDAPI WslcProcessGetState(_In_ WslcProcess process,
                           _Out_ WSLC_PROCESS_STATE* state);

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process,
                              _Out_ PINT32 exitCode);




// Will define more signals as needed:
typedef enum WSLC_PROCESS_SIGNAL
{
    WSLC_SIGNAL_NONE = 0,     // No signal; reserved for future use
    WSLC_SIGNAL_SIGHUP = 1,   // SIGHUP: reload / hangup
    WSLC_SIGNAL_SIGINT = 2,   // SIGINT: interrupt (Ctrl-C)
    WSLC_SIGNAL_SIGQUIT = 3,  // SIGQUIT: quit with core dump
    WSLC_SIGNAL_SIGKILL = 9,  // SIGKILL: immediate termination
    WSLC_SIGNAL_SIGTERM = 15, // SIGTERM: graceful shutdown
} WSLC_SIGNAL;

STDAPI WslcProcessSignal(WslcProcess process,
                         WSLC_PROCESS_SIGNAL signal);


STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process,
                               _In_ WSLC_PROCESS_IO_HANDLE ioHandle,
                               _Out_ HANDLE* handle);



STDAPI WslcProcessSettingsRelease(_In_ WslcProcessSettings processSettings);
STDAPI WslcProcessRelease(_In_ WslcProcess process);    

//IMAGE MANAGEMENT

// Container image
typedef struct WSLC_IMAGE_PROGRESS_DETAIL
{
    _Out_ uint64_t current; // bytes downloaded so far
    _Out_ uint64_t total;   // total bytes expected
} WSLC_IMAGE_PROGRESS_DETAIL;

typedef enum WSLC_IMAGE_PROGRESS_STATUS
{
    WSLC_IMAGE_PROGRESS_UNKNOWN = 0,
    WSLC_IMAGE_PROGRESS_PULLING,     // "Pulling fs layer"
    WSLC_IMAGE_PROGRESS_WAITING,     // "Waiting"
    WSLC_IMAGE_PROGRESS_DOWNLOADING, // "Downloading"
    WSLC_IMAGE_PROGRESS_VERIFYING,   // "Verifying Checksum"
    WSLC_IMAGE_PROGRESS_EXTRACTING,  // "Extracting"
    WSLC_IMAGE_PROGRESS_COMPLETE     // "Pull complete"
} WSLC_IMAGE_PROGRESS_STATUS;

typedef struct WSLC_IMAGE_PROGRESS_MESSAGE
{
    _Out_ PCSTR id;                          // layer ID or digest
    _Out_ WSLC_IMAGE_PROGRESS_STATUS status; // "Downloading", "Extracting", etc.
    _Out_ WSLC_IMAGE_PROGRESS_DETAIL detail;
} WSLC_IMAGE_PROGRESS_MESSAGE;

typedef __callback VOID(CALLBACK WslcContainerImageProgressCallback)(_In_ const WSLC_IMAGE_PROGRESS_MESSAGE* progress, _In_opt_ PVOID context);

typedef struct WSLC_REGISTRY_AUTHENTICATION_INFORMATION
{
    //TBD
} WSLC_REGISTRY_AUTHENTICATION_INFORMATION;

typedef struct WSLC_PULL_IMAGE_OPTIONS
{
    _In_z_ PCSTR uri; // e.g. "my.registry.io/hello-world:latest" or just "hello-world:latest" which will default to docker
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
    _In_opt_ const WSLC_REGISTRY_AUTHENTICATION_INFORMATION* authInfo;
} WSLC_PULL_IMAGE_OPTIONS;


STDAPI WslcSessionImagePull(_In_ WslcSession session,
                            _In_ const WSLC_PULL_IMAGE_OPTIONS* options,
                            _Outptr_opt_result_z_ PWSTR* errorMessage);


typedef struct WSLC_IMPORT_IMAGE_OPTIONS
{
    _In_z_ PCWSTR imagePath;
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WSLC_IMPORT_CONTAINER_IMAGE_OPTIONS;

STDAPI WslcSessionImageImport(_In_ WslcSession session,
                              _In_ const WSLC_IMPORT_CONTAINER_IMAGE_OPTIONS* options);

typedef struct WSLC_LOAD_IMAGE_OPTIONS
{
    HANDLE ImageHandle;
    UINT64 ContentLength;
    WslcContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
} WSLC_LOAD_CONTAINER_IMAGE_OPTIONS;


STDAPI WslcSessionImageLoad(_In_ WslcSession session,
                            _In_ const WSLC_LOAD_CONTAINER_IMAGE_OPTIONS* options);

typedef struct WSLC_IMAGE_INFO
{
    //we should expose this
    PCSTR repository; // e.g., "ubuntu"
    PCSTR tag;        // e.g., "22.04"
    UINT8 sha256[32]; // image digest (raw bytes)

    UINT64 sizeBytes;        // total size of the image
    UINT64 createdTimestamp; // Unix epoch (seconds or ms)
} WSLC_IMAGE_INFO;

STDAPI WslcSessionImageDelete(_In_ WslcSession session,
                              _In_reads_(32) const UINT8* sha256);

// Retrieves the list of container images
// Parameters:
//   session
//       A valid WslcSession handle.
//
//   images
//       On success, receives a pointer to a contiguous array of
//       WSLC_CONTAINER_IMAGE_INFO structures describing the images
//
//       The array is allocated using CoTaskMemAlloc. The caller takes
//       ownership of the memory and must free it by calling
//       CoTaskMemFree when it is no longer needed.
// 
//   count
//       On success, receives the number of elements in the images array.
//       On failure, *count is set to 0.
//
// Return Value:
//   S_OK on success. Otherwise, an HRESULT error code indicating the
//   reason for failure.
//
// Notes:
//   - The caller must pass non-null pointers for both 'images' and 'count'.
//
STDAPI WslcSessionImageList(_In_ WslcSession sesssion,
                            _Outptr_result_buffer_(*count) WSLC_IMAGE_INFO** images,
                            _Out_ UINT32* count);


// STORAGE

STDAPI WslcSessionCreateVhd(_In_ WslcSession sesssion,
                            _In_ const WSLC_VHD_REQUIREMENTS* options);

// INSTALL

typedef enum WSLC_COMPONENT_FLAGS
{
    WSLC_INSTALL_COMPONENT_NONE = 0,
    WSLC_INSTALL_COMPONENT_VMPOC = 1,
    WSLC_INSTALL_COMPONENT_WSL_OC = 2,
    WSLC_INSTALL_COMPONENT_WSL_PACKAGE = 4,
} WSLC_COMPONENT_FLAGS;



STDAPI WslcCanRun(_Out_ BOOL* canRun,
                  _Out_ WSLC_COMPONENT_FLAGS missingComponents);

typedef struct WSLC_VERSION
{
    UINT32 major;
    UINT32 minor;
    UINT32 revision;
} WSLC_VERSION;
STDAPI WslcGetVersion(_Out_writes_(1) WSLC_VERSION* version);


typedef __callback VOID(CALLBACK WslcInstallCallback)(_In_ WSLC_COMPONENT_FLAGS component,
                                                      _In_ UINT32 progress,
                                                      _In_ UINT32 total,
                                                      _In_opt_ PVOID context);

STDAPI WslcInstallWithDependencies(_In_opt_ __callback WslcInstallCallback progressCallback,
                                   _In_opt_ PVOID context);

EXTERN_C_END