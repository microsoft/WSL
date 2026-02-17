/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDK.h

Abstract:

    This file contains the public WSL Container SDK api definitions.

--*/
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <specstrings.h>

EXTERN_C_START

// Session values
#define WSLC_SESSION_OPTIONS_SIZE 72
#define WSLC_SESSION_OPTIONS_ALIGNMENT 8

typedef struct WslcSessionSettings
{
    __declspec(align(WSLC_SESSION_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_SESSION_OPTIONS_SIZE];
} WslcSessionSettings;

DECLARE_HANDLE(WslcSession);

// Container values
#define WSLC_CONTAINER_OPTIONS_SIZE 80
#define WSLC_CONTAINER_OPTIONS_ALIGNMENT 8

typedef struct WslcContainerSettings
{
    __declspec(align(WSLC_CONTAINER_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_OPTIONS_SIZE];
} WslcContainerSettings;

DECLARE_HANDLE(WslcContainer);

// Process values
#define WSLC_CONTAINER_PROCESS_OPTIONS_SIZE 48
#define WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT 8
typedef struct WslcProcessSettings
{
    __declspec(align(WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_PROCESS_OPTIONS_SIZE];
} WslcProcessSettings;

DECLARE_HANDLE(WslcProcess);

typedef enum WslcSessionNetworkingMode
{
    WSLC_SESSION_NETWORKING_MODE_NONE = 0, // No networking / isolated
    WSLC_SESSION_NETWORKING_MODE_NAT = 1,
    WSLC_SESSION_NETWORKING_MODE_VIRT_IO_PROXY = 2
} WslcSessionNetworkingMode;

typedef enum WslcContainerNetworkingMode
{
    WSLC_CONTAINER_NETWORKING_MODE_NONE = 0, // No networking / isolated
    WSLC_CONTAINER_NETWORKING_MODE_BRIDGED = 1
} WslcContainerNetworkingMode;

typedef enum WslcVhdType
{
    WSLC_VHD_TYPE_DYNAMIC = 0, // Expanding VHDX (default)
    WSLC_VHD_TYPE_FIXED = 1
} WslcVhdType;

typedef struct WslcVhdRequirements
{
    _In_ uint64_t sizeInBytes; // Desired size (for create/expand)
    _In_ WslcVhdType type;
} WslcVhdRequirements;

typedef enum WslcSessionFeatureFlags
{
    WSLC_SESSION_FEATURE_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU = 0x00000004
} WslcSessionFeatureFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcSessionFeatureFlags);

typedef enum WslcSessionFlags
{
    WSLC_SESSION_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FLAG_PERSISTENT = 0x00000001,
    WSLC_SESSION_FLAG_OPEN_EXISTING = 0x00000002
} WslcSessionFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcSessionFlags);

typedef enum WslcSessionTerminationReason
{
    WSLC_SESSION_TERMINATION_REASON_UNKNOWN = 0,
    WSLC_SESSION_TERMINATION_REASON_SHUTDOWN = 1,
    WSLC_SESSION_TERMINATION_REASON_CRASHED = 2,
} WslcSessionTerminationReason;

typedef __callback void(CALLBACK* WslcSessionTerminationCallback)(_In_ WslcSessionTerminationReason reason, _In_opt_ PVOID context);

STDAPI WslcSessionInitSettings(_In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings);

STDAPI WslcSessionCreate(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session);

// OPTIONAL SESSION SETTINGS
STDAPI WslcSessionSettingsSetCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount);
STDAPI WslcSessionSettingsSetMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMb);
STDAPI WslcSessionSettingsSetTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS);
STDAPI WslcSessionSettingsSetNetworkingMode(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionNetworkingMode mode);

STDAPI WslcSessionSettingsSetVHD(_In_ WslcSessionSettings* sessionSettings, _In_ const WslcVhdRequirements* vhdRequirements);

STDAPI WslcSessionSettingsSetFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags);
STDAPI WslcSessionSettingsSetFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFlags flags);

// Pass in Null for callback to clear the termination callback
STDAPI WslcSessionSettingsSetTerminateCallback(
    _In_ WslcSessionSettings* sessionSettings, _In_opt_ WslcSessionTerminationCallback terminationCallback, _In_opt_ PVOID terminationContext);

STDAPI WslcSessionTerminate(_In_ WslcSession session);
STDAPI WslcSessionRelease(_In_ WslcSession session);

// CONTAINER DEFINITIONS

typedef enum WslcPortProtocol
{
    WSLC_PORT_PROTOCOL_TCP = 0,
    WSLC_PORT_PROTOCOL_UDP = 1
} WslcPortProtocol;

typedef struct WslcContainerPortMapping
{

    _In_ uint16_t windowsPort;      // Port on Windows host
    _In_ uint16_t containerPort;    // Port inside container
    _In_ WslcPortProtocol protocol; // TCP or UDP

    // if you want to override the default binding address
    _In_opt_ struct sockaddr_storage* windowsAddress; // accepts ipv4/6
} WslcContainerPortMapping;

typedef struct WslcContainerVolume
{
    _In_z_ PCWSTR windowsPath;
    _In_z_ PCSTR containerPath;
    _In_ BOOL readOnly;
} WslcContainerVolume;

typedef enum WslcContainerFlags
{
    WSLC_CONTAINER_FLAG_NONE = 0x00000000,
    WSLC_CONTAINER_FLAG_AUTO_REMOVE = 0x00000001,
    WSLC_CONTAINER_FLAG_ENABLE_GPU = 0x00000002,
    WSLC_CONTAINER_FLAG_PRIVILEGED = 0x00000004,

} WslcContainerFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcContainerFlags);

STDAPI WslcContainerInitSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings);

STDAPI WslcContainerCreate(_In_ WslcSession session, _In_ WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcContainerStart(_In_ WslcContainer container);

// OPTIONAL CONTAINER SETTINGS
STDAPI WslcContainerSettingsSetName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name);

STDAPI WslcContainerSettingsSetInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess);

STDAPI WslcContainerSettingsSetNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode);

STDAPI WslcContainerSettingsSetHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName);

STDAPI WslcContainerSettingsSetDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName);

STDAPI WslcContainerSettingsSetFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags);

STDAPI WslcContainerSettingsSetPortMapping(
    _In_ WslcContainerSettings* containerSettings, _In_reads_(portMappingCount) const WslcContainerPortMapping* portMappings, _In_ uint32_t portMappingCount);

// Add the container volumes to the volumes array
STDAPI WslcContainerSettingsAddVolume(
    _In_ WslcContainerSettings* containerSettings, _In_reads_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount);

STDAPI WslcContainerExec(_In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess);

STDAPI WslcContainerRelease(_In_ WslcContainer container);

// GENERAL CONTAINER MANAGEMENT

#define WSLC_CONTAINER_ID_LENGTH 65 // 64 chars + null

STDAPI WslcContainerGetID(WslcContainer container, PCHAR (*containerId)[WSLC_CONTAINER_ID_LENGTH]);

STDAPI WslcContainerGetInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess);

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

typedef enum WslcContainerState
{
    WSLC_CONTAINER_STATE_INVALID = 0,
    WSLC_CONTAINER_STATE_CREATED = 1,
    WSLC_CONTAINER_STATE_RUNNING = 2,
    WSLC_CONTAINER_STATE_EXITED = 3,
    WSLC_CONTAINER_STATE_FAILED = 4,
} WslcContainerState;

STDAPI WslcContainerGetState(_In_ WslcContainer container, _Out_ WslcContainerState* state);

// Will define more signals as needed:
typedef enum WslcSignal
{
    WSLC_SIGNAL_NONE = 0,     // No signal; reserved for future use
    WSLC_SIGNAL_SIGHUP = 1,   // SIGHUP: reload / hangup
    WSLC_SIGNAL_SIGINT = 2,   // SIGINT: interrupt (Ctrl-C)
    WSLC_SIGNAL_SIGQUIT = 3,  // SIGQUIT: quit with core dump
    WSLC_SIGNAL_SIGKILL = 9,  // SIGKILL: immediate termination
    WSLC_SIGNAL_SIGTERM = 15, // SIGTERM: graceful shutdown
} WslcSignal;

STDAPI WslcContainerStop(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutMS);

typedef enum WslcDeleteContainerFlags
{
    WSLC_DELETE_CONTAINER_FLAG_NONE = 0,
    WSLC_DELETE_CONTAINER_FLAG_FORCE = 0x01
} WslcDeleteContainerFlags;

STDAPI WslcContainerDelete(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags);

// PROCESS DEFINITIONS
STDAPI WslcProcessInitSettings(_Out_ WslcProcessSettings* processSettings);

// OPTIONAL PROCESS SETTINGS

STDAPI WslcProcessSettingsSetExecutable(_In_ WslcProcessSettings* processSettings, _In_ PCSTR executable);

STDAPI WslcProcessSettingsSetCurrentDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR currentDirectory);

STDAPI WslcSessionSettingsSetDisplayName(_In_ WslcSessionSettings* sessionSettings, _In_ PCWSTR displayName);

STDAPI WslcProcessSettingsSetCmdLineArgs(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc);

STDAPI WslcProcessSettingsSetEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc);

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
typedef __callback void(CALLBACK* WslcStdIOCallback)(_In_reads_bytes_(dataSize) const BYTE* data, _In_ uint32_t dataSize, _In_opt_ PVOID context);
typedef enum WslcProcessIoHandle
{
    WSLC_PROCESS_IO_HANDLE_STDIN = 0,
    WSLC_PROCESS_IO_HANDLE_STDOUT = 1,
    WSLC_PROCESS_IO_HANDLE_STDERR = 2
} WslcProcessIoHandle;

// Pass in Null for WslcStdIOCallback to clear the callback for the given handle
STDAPI WslcProcessSettingsSetIoCallback(
    _In_ WslcProcessSettings* processSettings, _In_ WslcProcessIoHandle ioHandle, _In_opt_ WslcStdIOCallback stdIOCallback, _In_opt_ PVOID context);

// PROCESS MANAGEMENT

STDAPI WslcProcessGetPid(_In_ WslcProcess process, _Out_ uint32_t* pid);

STDAPI WslcProcessGetExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent);

typedef enum WslcProcessState
{
    WSLC_PROCESS_STATE_UNKNOWN = 0,
    WSLC_PROCESS_STATE_CREATED = 1,
    WSLC_PROCESS_STATE_RUNNING = 2,
    WSLC_PROCESS_STATE_EXITED = 3
} WslcProcessState;

STDAPI WslcProcessGetState(_In_ WslcProcess process, _Out_ WslcProcessState* state);

STDAPI WslcProcessGetExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode);

STDAPI WslcProcessSignal(_In_ WslcProcess process, _In_ WslcSignal signal);

STDAPI WslcProcessGetIOHandles(_In_ WslcProcess process, _In_ WslcProcessIoHandle ioHandle, _Out_ HANDLE* handle);

STDAPI WslcProcessRelease(_In_ WslcProcess process);

// IMAGE MANAGEMENT

// Container image
typedef struct WslcImageProgressDetail
{
    _Out_ uint64_t current; // bytes downloaded so far
    _Out_ uint64_t total;   // total bytes expected
} WslcImageProgressDetail;

typedef enum WslcImageProgressStatus
{
    WSLC_IMAGE_PROGRESS_STATUS_UNKNOWN = 0,
    WSLC_IMAGE_PROGRESS_STATUS_PULLING = 1,     // "Pulling fs layer"
    WSLC_IMAGE_PROGRESS_STATUS_WAITING = 2,     // "Waiting"
    WSLC_IMAGE_PROGRESS_STATUS_DOWNLOADING = 3, // "Downloading"
    WSLC_IMAGE_PROGRESS_STATUS_VERIFYING = 4,   // "Verifying Checksum"
    WSLC_IMAGE_PROGRESS_STATUS_EXTRACTING = 5,  // "Extracting"
    WSLC_IMAGE_PROGRESS_STATUS_COMPLETE = 6     // "Pull complete"
} WslcImageProgressStatus;

typedef struct WslcImageProgressMessage
{
    _Out_ PCSTR id;                       // layer ID or digest
    _Out_ WslcImageProgressStatus status; // "Downloading", "Extracting", etc.
    _Out_ WslcImageProgressDetail detail;
} WslcImageProgressMessage;

typedef struct WslcRegistryAuthenticationInformation
{
    // TBD
} WslcRegistryAuthenticationInformation;

// pointer-to-function typedef (unambiguous)
typedef HRESULT(CALLBACK* WslcContainerImageProgressCallback)(const WslcImageProgressMessage* progress, PVOID context);

// options struct typedef is a pointer type and _In_opt_ is valid
typedef struct WslcPullImageOptions
{
    _In_z_ PCSTR uri;
    WslcContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
    _In_opt_ const WslcRegistryAuthenticationInformation* authInfo;
} WslcPullImageOptions;

STDAPI WslcSessionImagePull(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef struct WslcImportImageOptions
{
    _In_z_ PCWSTR imagePath;
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcImportImageOptions;

STDAPI WslcSessionImageImport(_In_ WslcSession session, _In_ const WslcImportImageOptions* options);

typedef struct WslcLoadImageOptions
{
    HANDLE ImageHandle;
    uint64_t ContentLength;
    WslcContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
} WslcLoadImageOptions;

STDAPI WslcSessionImageLoad(_In_ WslcSession session, _In_ const WslcLoadImageOptions* options);

typedef struct WslcImageInfo
{
    // we should expose this
    PCSTR repository;
    PCSTR tag;
    uint8_t sha256[32];
    uint64_t sizeBytes;
    uint64_t createdTimestamp;
} WslcImageInfo;

STDAPI WslcSessionImageDelete(_In_ WslcSession session, _In_z_ PCSTR NameOrId);

// Retrieves the list of container images
// Parameters:
//   session
//       A valid WslcSession handle.
//
//   images
//       On success, receives a pointer to a contiguous array of
//       WslcImageInfo structures describing the images
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

STDAPI WslcSessionImageList(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count);

// STORAGE

STDAPI WslcSessionCreateVhd(_In_ WslcSession session, _In_ const WslcVhdRequirements* options);

// INSTALL

typedef enum WslcComponentFlags
{
    WSLC_COMPONENT_FLAG_NONE = 0,
    WSLC_COMPONENT_FLAG_VMPOC = 1,
    WSLC_COMPONENT_FLAG_WSL_OC = 2,
    WSLC_COMPONENT_FLAG_WSL_PACKAGE = 4,
} WslcComponentFlags;

STDAPI WslcCanRun(_Out_ BOOL* canRun, _Out_ WslcComponentFlags* missingComponents);

typedef struct WslcVersion
{
    uint32_t major;
    uint32_t minor;
    uint32_t revision;
} WslcVersion;
STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version);

typedef __callback void(CALLBACK* WslcInstallCallback)(_In_ WslcComponentFlags component, _In_ uint32_t progress, _In_ uint32_t total, _In_opt_ PVOID context);

STDAPI WslcInstallWithDependencies(_In_opt_ WslcInstallCallback progressCallback, _In_opt_ PVOID context);

EXTERN_C_END
