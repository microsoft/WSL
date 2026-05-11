/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslPluginApi.h

Abstract:

    This file contains the interface for WSL plugins to interact with WSL distributions.

--*/

#pragma once

#include <stdint.h>

// Must be lowercase. See: https://github.com/microsoft/WSL/issues/12580
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WSLPLUGINAPI_ENTRYPOINTV1 WSLPluginAPIV1_EntryPoint
#define WSL_E_PLUGIN_REQUIRES_UPDATE MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x032A)

#define WSL_PLUGIN_REQUIRE_VERSION(_Major, _Minor, _Revision, Api) \
    if (Api->Version.Major < (_Major) || (Api->Version.Major == (_Major) && Api->Version.Minor < (_Minor)) || \
        (Api->Version.Major == (_Major) && Api->Version.Minor == (_Minor) && Api->Version.Revision < (_Revision))) \
    { \
        return WSL_E_PLUGIN_REQUIRES_UPDATE; \
    }

struct WSLVersion
{
    uint32_t Major;
    uint32_t Minor;
    uint32_t Revision;
};

enum WSLUserConfiguration
{
    None = 0,
    WSLUserConfigurationCustomKernel = 1,
    WSLUserConfigurationCustomKernelCommandLine = 2
};

#ifdef __cplusplus
DEFINE_ENUM_FLAG_OPERATORS(WSLUserConfiguration);
#endif

struct WSLVmCreationSettings
{
    enum WSLUserConfiguration CustomConfigurationFlags;
};

typedef DWORD WSLSessionId;

struct WSLSessionInformation
{
    WSLSessionId SessionId;
    HANDLE UserToken;
    PSID UserSid;
};

struct WSLDistributionInformation
{
    GUID Id; // Distribution ID, guaranteed to be the same across reboots
    LPCWSTR Name;
    uint64_t PidNamespace;
    LPCWSTR PackageFamilyName; // Package family name, or NULL if none
    uint32_t InitPid;          // Pid of the init process. Introduced in 2.0.5
    LPCWSTR Flavor;            // Type of distribution (ubuntu, debian, ...). Introduced in 2.4.4
    LPCWSTR Version;           // Distribution version. Introduced in 2.4.4
};

struct WslOfflineDistributionInformation
{
    GUID Id; // Distribution ID, guaranteed to be the same across reboots
    LPCWSTR Name;
    LPCWSTR PackageFamilyName; // Package family name, or NULL if none
    LPCWSTR Flavor;            // Type of distribution (ubuntu, debian, ...). Introduced in 2.4.4
    LPCWSTR Version;           // Distribution version. Introduced in 2.4.4
};

// Identifies a WSLC session inside the WSLC plugin API. Distinct from WSLSessionId.
typedef DWORD WSLCSessionId;

// Information about a WSLC session passed to plugin notifications.
struct WSLCSessionInformation
{
    WSLCSessionId SessionId;
    LPCWSTR DisplayName;
    DWORD ApplicationPid;
    HANDLE UserToken;
    PSID UserSid;
};

// Represents a running WSLC process started by the plugin.
// Stdin, Stdout and Stderr are connected to the process' stdio.
// The caller must close the sockets and 'ExitEvent'.
struct WSLCProcess
{
    ULONG Pid;
    SOCKET Stdin;
    SOCKET Stdout;
    SOCKET Stderr;
    HANDLE ExitEvent; // Signaled when the process exits.
};

// Create plan9 mount between Windows & Linux
typedef HRESULT (*WSLPluginAPI_MountFolder)(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name);

// Execute a program in the root namespace.
// On success, 'Socket' is connected to stdin & stdout (stderr goes to dmesg) // 'Arguments' is expected to be NULL terminated
typedef HRESULT (*WSLPluginAPI_ExecuteBinary)(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

//
// WSLC plugin hooks (called by WSL into the plugin)
//

// Called when a WSLC session is created. Returning an error prevents the session creation.
typedef HRESULT (*WSLPluginAPI_OnSessionCreated)(const struct WSLCSessionInformation* Session);

// Called when a WSLC session is about to stop. Errors are ignored.
typedef HRESULT (*WSLPluginAPI_OnSessionStopping)(const struct WSLCSessionInformation* Session);

// Called when a container starts. Returning an error prevents the container creation.
// 'InspectContainer' is a JSON document that follows the wslc_schema::InspectContainer format.
typedef HRESULT (*WSLPluginAPI_ContainerStarted)(const struct WSLCSessionInformation* Session, LPCSTR InspectContainer);

// Called when a container is about to stop. Errors are ignored.
typedef HRESULT (*WSLPluginAPI_ContainerStopping)(const struct WSLCSessionInformation* Session, LPCSTR InspectContainer);

// Called when an image is created (either pulled, or imported). Errors are ignored.
// 'InspectImage' is a JSON document that follows the wslc_schema::InspectImage format.
typedef HRESULT (*WSLPluginAPI_ImageCreated)(const struct WSLCSessionInformation* Session, LPCSTR InspectImage);

// Called when an image is deleted. Errors are ignored.
typedef HRESULT (*WSLPluginAPI_ImageDeleted)(const struct WSLCSessionInformation* Session, LPCSTR InspectImage);

//
// WSLC plugin -> API calls (called by the plugin into WSL)
//

// Mount a Windows folder into the WSLC session VM. The mount path is returned via 'Mountpoint'.
// 'Mountpoint' must point to a buffer of at least 256 wchar_t.
typedef HRESULT (*WSLCPluginAPI_MountFolder)(WSLCSessionId Session, LPCWSTR WindowsPath, BOOL ReadOnly, LPCWSTR Name, LPWSTR Mountpoint);

// Unmount a folder previously mounted via WSLCPluginAPI_MountFolder.
typedef HRESULT (*WSLCPluginAPI_UnmountFolder)(WSLCSessionId Session, LPCWSTR Mountpoint);

// Create a process in the WSLC session's root namespace.
// 'Arguments' and 'Env' are NULL-terminated arrays. 'Env' may be NULL.
typedef HRESULT (*WSLCPluginAPI_CreateProcess)(
    WSLCSessionId Session, LPCSTR Executable, LPCSTR* Arguments, LPCSTR* Env, struct WSLCProcess* Process);

// Wait for a process previously created via WSLCPluginAPI_CreateProcess to exit and get its exit code.
typedef HRESULT (*WSLCPluginAPI_WaitPid)(WSLCSessionId Session, LONG Pid, ULONGLONG Timeout, int* Status);

// Execute a program in a user distribution
// On success, 'Socket' is connected to stdin & stdout (stderr goes to dmesg) // 'Arguments' is expected to be NULL terminated
typedef HRESULT (*WSLPluginAPI_ExecuteBinaryInDistribution)(WSLSessionId Session, const GUID* Distribution, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

// Set the error message to display to the user if the VM or distribution creation fails.
// Must be called synchronously in either OnVMStarted() or OnDistributionStarted().
typedef HRESULT (*WSLPluginAPI_PluginError)(LPCWSTR UserMessage);

// Synchronous notifications sent to the plugin

// Called when the VM has started.
// 'Session' and 'UserSettings' are only valid during while the call is in progress.
typedef HRESULT (*WSLPluginAPI_OnVMStarted)(const struct WSLSessionInformation* Session, const struct WSLVmCreationSettings* UserSettings);

// Called when the VM is about to stop.
// 'Session' is only valid during while the call is in progress.
typedef HRESULT (*WSLPluginAPI_OnVMStopping)(const struct WSLSessionInformation* Session);

// Called when a distribution has started.
// 'Session' and 'Distribution' is only valid during while the call is in progress.
typedef HRESULT (*WSLPluginAPI_OnDistributionStarted)(const struct WSLSessionInformation* Session, const struct WSLDistributionInformation* Distribution);

// Called when a distribution is about to stop.
// 'Session' and 'Distribution' is only valid during while the call is in progress.
// Note: It's possible that stopping a distribution fails (for instance if a file is in use).
// In this case, it's possible for this notification to be called multiple times for the same distribution.
typedef HRESULT (*WSLPluginAPI_OnDistributionStopping)(const struct WSLSessionInformation* Session, const struct WSLDistributionInformation* Distribution);

// Called when a distribution is registered or unregistered.
// Returning failure will NOT cause the operation to fail.
typedef HRESULT (*WSLPluginAPI_OnDistributionRegistered)(const struct WSLSessionInformation* Session, const struct WslOfflineDistributionInformation* Distribution);

// WSLC plugin -> API surface. Pointed to by WSLPluginAPIV1::Wslc.
struct WSLCPluginAPIV1
{
    WSLCPluginAPI_MountFolder MountFolder;
    WSLCPluginAPI_UnmountFolder UnmountFolder;
    WSLCPluginAPI_CreateProcess CreateProcess;
    WSLCPluginAPI_WaitPid WaitPid;
};

struct WSLPluginHooksV1
{
    WSLPluginAPI_OnVMStarted OnVMStarted;
    WSLPluginAPI_OnVMStopping OnVMStopping;
    WSLPluginAPI_OnDistributionStarted OnDistributionStarted;
    WSLPluginAPI_OnDistributionStopping OnDistributionStopping;
    WSLPluginAPI_OnDistributionRegistered OnDistributionRegistered;   // Introduced in 2.1.2
    WSLPluginAPI_OnDistributionRegistered OnDistributionUnregistered; // Introduced in 2.1.2

    // WSLC hooks. Plugins compiled against older headers leave these zero-initialized.
    WSLPluginAPI_OnSessionCreated OnSessionCreated;
    WSLPluginAPI_OnSessionStopping OnSessionStopping;
    WSLPluginAPI_ContainerStarted ContainerStarted;
    WSLPluginAPI_ContainerStopping ContainerStopping;
    WSLPluginAPI_ImageCreated ImageCreated;
    WSLPluginAPI_ImageDeleted ImageDeleted;
};

struct WSLPluginAPIV1
{
    struct WSLVersion Version;
    WSLPluginAPI_MountFolder MountFolder;
    WSLPluginAPI_ExecuteBinary ExecuteBinary;
    WSLPluginAPI_PluginError PluginError;
    WSLPluginAPI_ExecuteBinaryInDistribution ExecuteBinaryInDistribution; // Introduced in 2.1.2

    // WSLC plugin -> API surface. NULL if loaded against an older WSL.
    const struct WSLCPluginAPIV1* Wslc;
};

typedef HRESULT (*WSLPluginAPI_EntryPointV1)(const struct WSLPluginAPIV1* Api, struct WSLPluginHooksV1* Hooks);

#ifdef __cplusplus
}
#endif