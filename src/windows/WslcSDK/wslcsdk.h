/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDK.h

Abstract:

    This file contains the public WSL Container SDK api definitions.

    PREVIEW NOTICE: This API is currently in preview and is subject to breaking
    changes in future releases without prior notice. Do not rely on API stability
    for production workloads. Features, function signatures, and behaviors may
    change between releases during the preview period.

--*/
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <specstrings.h>

EXTERN_C_START

// Session values
#define WSLC_SESSION_OPTIONS_SIZE 80
#define WSLC_SESSION_OPTIONS_ALIGNMENT 8

typedef struct WslcSessionSettings
{
    __declspec(align(WSLC_SESSION_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_SESSION_OPTIONS_SIZE];
} WslcSessionSettings;

DECLARE_HANDLE(WslcSession);

// Container values
#define WSLC_CONTAINER_OPTIONS_SIZE 96
#define WSLC_CONTAINER_OPTIONS_ALIGNMENT 8

typedef struct WslcContainerSettings
{
    __declspec(align(WSLC_CONTAINER_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_OPTIONS_SIZE];
} WslcContainerSettings;

DECLARE_HANDLE(WslcContainer);

// Process values
#define WSLC_CONTAINER_PROCESS_OPTIONS_SIZE 72
#define WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT 8
typedef struct WslcProcessSettings
{
    __declspec(align(WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT)) BYTE _opaque[WSLC_CONTAINER_PROCESS_OPTIONS_SIZE];
} WslcProcessSettings;

DECLARE_HANDLE(WslcProcess);

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
    // Ignored by WslcSetSessionSettingsVhd
    _In_z_ PCSTR name;
    _In_ uint64_t sizeBytes; // Desired size (for create/expand)
    _In_ WslcVhdType type;
} WslcVhdRequirements;

typedef enum WslcSessionFeatureFlags
{
    WSLC_SESSION_FEATURE_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU = 0x00000004
} WslcSessionFeatureFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcSessionFeatureFlags);

typedef enum WslcSessionTerminationReason
{
    WSLC_SESSION_TERMINATION_REASON_UNKNOWN = 0,
    WSLC_SESSION_TERMINATION_REASON_SHUTDOWN = 1,
    WSLC_SESSION_TERMINATION_REASON_CRASHED = 2,
} WslcSessionTerminationReason;

typedef __callback void(CALLBACK* WslcSessionTerminationCallback)(_In_ WslcSessionTerminationReason reason, _In_opt_ PVOID context);

STDAPI WslcInitSessionSettings(_In_ PCWSTR name, _In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings);

STDAPI WslcCreateSession(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session, _Outptr_opt_result_z_ PWSTR* errorMessage);

// OPTIONAL SESSION SETTINGS
STDAPI WslcSetSessionSettingsCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount);
STDAPI WslcSetSessionSettingsMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMB);
STDAPI WslcSetSessionSettingsTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS);

STDAPI WslcSetSessionSettingsVhd(_In_ WslcSessionSettings* sessionSettings, _In_opt_ const WslcVhdRequirements* vhdRequirements);

STDAPI WslcSetSessionSettingsFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags);

// Pass in Null for callback to clear the termination callback
STDAPI WslcSetSessionSettingsTerminationCallback(
    _In_ WslcSessionSettings* sessionSettings, _In_opt_ WslcSessionTerminationCallback terminationCallback, _In_opt_ PVOID terminationContext);

STDAPI WslcTerminateSession(_In_ WslcSession session);
STDAPI WslcReleaseSession(_In_ WslcSession session);

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

typedef struct WslcContainerNamedVolume
{
    _In_z_ PCSTR name;          // Name of the session volume (from WslcVhdRequirements.name)
    _In_z_ PCSTR containerPath; // Absolute path inside the container
    _In_ BOOL readOnly;
} WslcContainerNamedVolume;

typedef enum WslcContainerFlags
{
    WSLC_CONTAINER_FLAG_NONE = 0x00000000,
    WSLC_CONTAINER_FLAG_AUTO_REMOVE = 0x00000001,
    WSLC_CONTAINER_FLAG_ENABLE_GPU = 0x00000002,
    WSLC_CONTAINER_FLAG_PRIVILEGED = 0x00000004,

} WslcContainerFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcContainerFlags);

typedef enum WslcContainerStartFlags
{
    WSLC_CONTAINER_START_FLAG_NONE = 0x00000000,
    WSLC_CONTAINER_START_FLAG_ATTACH = 0x00000001,

} WslcContainerStartFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcContainerStartFlags);

STDAPI WslcInitContainerSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings);

STDAPI WslcCreateContainer(_In_ WslcSession session, _In_ const WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcStartContainer(_In_ WslcContainer container, _In_ WslcContainerStartFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage);

// OPTIONAL CONTAINER SETTINGS
STDAPI WslcSetContainerSettingsName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name);

STDAPI WslcSetContainerSettingsInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess);

STDAPI WslcSetContainerSettingsNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode);

STDAPI WslcSetContainerSettingsHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName);

STDAPI WslcSetContainerSettingsDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName);

STDAPI WslcSetContainerSettingsFlags(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerFlags flags);

STDAPI WslcSetContainerSettingsPortMappings(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(portMappingCount) const WslcContainerPortMapping* portMappings,
    _In_ uint32_t portMappingCount);

// Add the container volumes to the volumes array
STDAPI WslcSetContainerSettingsVolumes(
    _In_ WslcContainerSettings* containerSettings, _In_reads_opt_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount);

// Add named session volumes (created via WslcCreateSessionVhdVolume) to the container settings
STDAPI WslcSetContainerSettingsNamedVolumes(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(namedVolumeCount) const WslcContainerNamedVolume* namedVolumes,
    _In_ uint32_t namedVolumeCount);

STDAPI WslcCreateContainerProcess(
    _In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess, _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcReleaseContainer(_In_ WslcContainer container);

// GENERAL CONTAINER MANAGEMENT

#define WSLC_CONTAINER_ID_BUFFER_SIZE 65 // 64 hex chars + null terminator

STDAPI WslcGetContainerID(_In_ WslcContainer container, _Out_writes_(WSLC_CONTAINER_ID_BUFFER_SIZE) CHAR containerID[WSLC_CONTAINER_ID_BUFFER_SIZE]);

STDAPI WslcGetContainerInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess);

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
STDAPI WslcInspectContainer(_In_ WslcContainer container, _Outptr_result_z_ PSTR* inspectData);

typedef enum WslcContainerState
{
    WSLC_CONTAINER_STATE_INVALID = 0,
    WSLC_CONTAINER_STATE_CREATED = 1,
    WSLC_CONTAINER_STATE_RUNNING = 2,
    WSLC_CONTAINER_STATE_EXITED = 3,
    WSLC_CONTAINER_STATE_DELETED = 4,
} WslcContainerState;

STDAPI WslcGetContainerState(_In_ WslcContainer container, _Out_ WslcContainerState* state);

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

STDAPI WslcStopContainer(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutSeconds, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef enum WslcDeleteContainerFlags
{
    WSLC_DELETE_CONTAINER_FLAG_NONE = 0,
    WSLC_DELETE_CONTAINER_FLAG_FORCE = 0x01
} WslcDeleteContainerFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcDeleteContainerFlags);

STDAPI WslcDeleteContainer(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage);

// PROCESS DEFINITIONS
STDAPI WslcInitProcessSettings(_Out_ WslcProcessSettings* processSettings);

// OPTIONAL PROCESS SETTINGS

STDAPI WslcSetProcessSettingsWorkingDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR workingDirectory);

STDAPI WslcSetProcessSettingsCmdLine(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc);

STDAPI WslcSetProcessSettingsEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc);

typedef enum WslcProcessIOHandle
{
    WSLC_PROCESS_IO_HANDLE_STDIN = 0,
    WSLC_PROCESS_IO_HANDLE_STDOUT = 1,
    WSLC_PROCESS_IO_HANDLE_STDERR = 2
} WslcProcessIOHandle;

// Callback invoked when stdout or stderr data is available from a running
// WSLC process.
//
// Parameters:
//   ioHandle
//       The WslcProcessIOHandle that the IO callback is for.
//       Only STDOUT and STDERR will receive callbacks.
//
//   data
//       Pointer to a buffer containing the bytes read. The buffer is owned
//       by WSLC and is valid only for the duration of the callback.
//
//       The caller must not free, modify, or retain the pointer. If the
//       caller needs to keep the data, it must copy the contents before
//       returning from the callback.
//
//   dataBytes
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
typedef __callback void(CALLBACK* WslcStdIOCallback)(
    WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context);

// Callback invoked when a WSLC process has exited AND any remaining IO has been flushed.
//
// Parameters:
//   exitCode
//       The exit code of the process.
//
//   context
//       Caller-supplied context pointer that was provided when the callback
//       was registered.
//
// Notes:
//   - Once this callback is invoked, any registered IO callbacks will no longer be called.
//
typedef __callback void(CALLBACK* WslcProcessExitCallback)(INT32 exitCode, _In_opt_ PVOID context);

// Using any callbacks will consume the IO handles, preventing acquisition through WslcGetProcessIOHandle.
// If using IO callbacks, also use the exit callback to prevent a race between process exit and IO buffer flushing.
typedef struct WslcProcessCallbacks
{
    WslcStdIOCallback onStdOut;
    WslcStdIOCallback onStdErr;
    WslcProcessExitCallback onExit;
} WslcProcessCallbacks;

STDAPI WslcSetProcessSettingsCallbacks(_In_ WslcProcessSettings* processSettings, _In_ const WslcProcessCallbacks* callbacks, _In_opt_ PVOID context);

// PROCESS MANAGEMENT

STDAPI WslcGetProcessPid(_In_ WslcProcess process, _Out_ uint32_t* pid);

STDAPI WslcGetProcessExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent);

typedef enum WslcProcessState
{
    WSLC_PROCESS_STATE_UNKNOWN = 0,
    WSLC_PROCESS_STATE_RUNNING = 1,
    WSLC_PROCESS_STATE_EXITED = 2,
    WSLC_PROCESS_STATE_SIGNALLED = 3
} WslcProcessState;

STDAPI WslcGetProcessState(_In_ WslcProcess process, _Out_ WslcProcessState* state);

STDAPI WslcGetProcessExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode);

STDAPI WslcSignalProcess(_In_ WslcProcess process, _In_ WslcSignal signal);

STDAPI WslcGetProcessIOHandle(_In_ WslcProcess process, _In_ WslcProcessIOHandle ioHandle, _Out_ HANDLE* handle);

STDAPI WslcReleaseProcess(_In_ WslcProcess process);

// IMAGE MANAGEMENT

// Container image
typedef struct WslcImageProgressDetail
{
    _Out_ uint64_t currentBytes; // bytes downloaded so far
    _Out_ uint64_t totalBytes;   // total bytes expected
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

// pointer-to-function typedef (unambiguous)
typedef HRESULT(CALLBACK* WslcContainerImageProgressCallback)(const WslcImageProgressMessage* progress, PVOID context);

// options struct typedef is a pointer type and _In_opt_ is valid
typedef struct WslcPullImageOptions
{
    _In_z_ PCSTR uri;
    WslcContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
    _In_opt_z_ PCSTR registryAuth;
} WslcPullImageOptions;

STDAPI WslcPullSessionImage(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef struct WslcImportImageOptions
{
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcImportImageOptions;

STDAPI WslcImportSessionImage(
    _In_ WslcSession session,
    _In_z_ PCSTR imageName,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentBytes,
    _In_opt_ const WslcImportImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcImportSessionImageFromFile(
    _In_ WslcSession session, _In_z_ PCSTR imageName, _In_z_ PCWSTR path, _In_opt_ const WslcImportImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef struct WslcLoadImageOptions
{
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcLoadImageOptions;

STDAPI WslcLoadSessionImage(
    _In_ WslcSession session,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentBytes,
    _In_opt_ const WslcLoadImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage);

STDAPI WslcLoadSessionImageFromFile(
    _In_ WslcSession session, _In_z_ PCWSTR path, _In_opt_ const WslcLoadImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

#define WSLC_IMAGE_NAME_LENGTH 256 // 255 chars + null

typedef struct WslcImageInfo
{
    // we should expose this
    CHAR name[WSLC_IMAGE_NAME_LENGTH];
    uint8_t sha256[32];
    uint64_t sizeBytes;
    uint64_t createdUnixTime;
} WslcImageInfo;

STDAPI WslcDeleteSessionImage(_In_ WslcSession session, _In_z_ PCSTR nameOrID, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef struct WslcTagImageOptions
{
    _In_z_ PCSTR image; // Source image name or ID.
    _In_z_ PCSTR repo;  // Target repository name.
    _In_z_ PCSTR tag;   // Target tag name.
} WslcTagImageOptions;

STDAPI WslcTagSessionImage(_In_ WslcSession session, _In_ const WslcTagImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

typedef struct WslcPushImageOptions
{
    _In_z_ PCSTR image;
    _In_z_ PCSTR registryAuth; // Base64-encoded X-Registry-Auth header value.
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcPushImageOptions;

STDAPI WslcPushSessionImage(_In_ WslcSession session, _In_ const WslcPushImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);

// Authenticates with a container registry and returns an identity token.
//
// Parameters:
//   session
//       A valid WslcSession handle.
//
//   serverAddress
//       The registry server address (e.g. "127.0.0.1:5000").
//
//   username
//       The username for authentication.
//
//   password
//       The password for authentication.
//
//   identityToken
//       On success, receives a pointer to a null-terminated ANSI string
//       containing the identity token.
//
//       The string is allocated using CoTaskMemAlloc. The caller takes
//       ownership of the returned memory and must free it by calling
//       CoTaskMemFree when it is no longer needed.
//
// Return Value:
//   S_OK on success. Otherwise, an HRESULT error code indicating the failure.
STDAPI WslcSessionAuthenticate(
    _In_ WslcSession session,
    _In_z_ PCSTR serverAddress,
    _In_z_ PCSTR username,
    _In_z_ PCSTR password,
    _Outptr_result_z_ PSTR* identityToken,
    _Outptr_opt_result_z_ PWSTR* errorMessage);

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

STDAPI WslcListSessionImages(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count);

// STORAGE

STDAPI WslcCreateSessionVhdVolume(_In_ WslcSession session, _In_ const WslcVhdRequirements* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
STDAPI WslcDeleteSessionVhdVolume(_In_ WslcSession session, _In_z_ PCSTR name, _Outptr_opt_result_z_ PWSTR* errorMessage);

// INSTALL

typedef enum WslcComponentFlags
{
    WSLC_COMPONENT_FLAG_NONE = 0,
    // Services provided by the Virtual Machine Platform optional feature (other optional features may provide these services as
    // well). Installing this component will require a reboot.
    WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM = 1,
    // The WSL runtime package, at an appropriate version to provide support for WSLC.
    WSLC_COMPONENT_FLAG_WSL_PACKAGE = 2,
} WslcComponentFlags;

DEFINE_ENUM_FLAG_OPERATORS(WslcComponentFlags);

STDAPI WslcGetMissingComponents(_Out_ WslcComponentFlags* missingComponents);

typedef struct WslcVersion
{
    uint32_t major;
    uint32_t minor;
    uint32_t revision;
} WslcVersion;
STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version);

typedef __callback void(CALLBACK* WslcInstallCallback)(_In_ WslcComponentFlags component, _In_ uint32_t progressSteps, _In_ uint32_t totalSteps, _In_opt_ PVOID context);

STDAPI WslcInstallWithDependencies(_In_opt_ WslcInstallCallback progressCallback, _In_opt_ PVOID context);

EXTERN_C_END
