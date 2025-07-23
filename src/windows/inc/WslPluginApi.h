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
#define WSL_E_PLUGIN_REQUIRES_UPDATE MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x8004032A)

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

// Create plan9 mount between Windows & Linux
typedef HRESULT (*WSLPluginAPI_MountFolder)(WSLSessionId Session, LPCWSTR WindowsPath, LPCWSTR LinuxPath, BOOL ReadOnly, LPCWSTR Name);

// Execute a program in the root namespace.
// On success, 'Socket' is connected to stdin & stdout (stderr goes to dmesg) // 'Arguments' is expected to be NULL terminated
typedef HRESULT (*WSLPluginAPI_ExecuteBinary)(WSLSessionId Session, LPCSTR Path, LPCSTR* Arguments, SOCKET* Socket);

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

struct WSLPluginHooksV1
{
    WSLPluginAPI_OnVMStarted OnVMStarted;
    WSLPluginAPI_OnVMStopping OnVMStopping;
    WSLPluginAPI_OnDistributionStarted OnDistributionStarted;
    WSLPluginAPI_OnDistributionStopping OnDistributionStopping;
    WSLPluginAPI_OnDistributionRegistered OnDistributionRegistered;   // Introduced in 2.1.2
    WSLPluginAPI_OnDistributionRegistered OnDistributionUnregistered; // Introduced in 2.1.2
};

struct WSLPluginAPIV1
{
    struct WSLVersion Version;
    WSLPluginAPI_MountFolder MountFolder;
    WSLPluginAPI_ExecuteBinary ExecuteBinary;
    WSLPluginAPI_PluginError PluginError;
    WSLPluginAPI_ExecuteBinaryInDistribution ExecuteBinaryInDistribution; // Introduced in 2.1.2
};

typedef HRESULT (*WSLPluginAPI_EntryPointV1)(const struct WSLPluginAPIV1* Api, struct WSLPluginHooksV1* Hooks);

#ifdef __cplusplus
}
#endif