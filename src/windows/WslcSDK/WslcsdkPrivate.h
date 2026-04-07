/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcSDKPrivate.h

Abstract:

    This file contains the private WSL Container SDK definitions.

--*/
#pragma once
#include <windows.h>
#include "wslcsdk.h"
#include "wslc.h"
#include "IOCallback.h"
#include <stdint.h>
#include <wil/com.h> // COM helpers
// #include <wil/resource.h> // handle wrappers
// #include <wil/result.h>   // error handling

// SESSION DEFINITIONS
typedef struct WslcSessionOptionsInternal
{
    PCWSTR displayName;
    PCWSTR storagePath;

    uint32_t cpuCount;
    uint32_t memoryMb;
    uint32_t timeoutMS;

    WslcVhdRequirements vhdRequirements;
    WslcSessionFeatureFlags featureFlags;
    WslcSessionTerminationCallback terminationCallback;
    PVOID terminationCallbackContext;
} WslcSessionOptionsInternal;

static_assert(sizeof(WslcSessionOptionsInternal) == WSLC_SESSION_OPTIONS_SIZE, "WSLC_SESSION_OPTIONS_INTERNAL size mismatch");

static_assert(
    __alignof(WslcSessionOptionsInternal) == WSLC_SESSION_OPTIONS_ALIGNMENT, "WSLC_SESSION_OPTIONS_INTERNAL alignment mismatch");

static_assert(std::is_trivial_v<WslcSessionOptionsInternal>, "WSLC_SESSION_OPTIONS_INTERNAL must be trivial");

WslcSessionOptionsInternal* GetInternalType(WslcSessionSettings* settings);

struct WslcContainerProcessIOCallbackOptions : public WslcProcessCallbacks
{
    PVOID callbackContext;
};

// PROCESS DEFINITIONS
typedef struct WslcContainerProcessOptionsInternal
{
    PCSTR const* commandLine;
    uint32_t commandLineCount;
    PCSTR const* environment;
    uint32_t environmentCount;
    PCSTR currentDirectory;
    WslcContainerProcessIOCallbackOptions ioCallbacks;
} WslcContainerProcessOptionsInternal;

static_assert(
    sizeof(WslcContainerProcessOptionsInternal) == WSLC_CONTAINER_PROCESS_OPTIONS_SIZE,
    "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL size mismatch");
static_assert(
    __alignof(WslcContainerProcessOptionsInternal) == WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT,
    "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL alignment mismatch");

static_assert(std::is_trivial_v<WslcContainerProcessOptionsInternal>, "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL must be trivial");

WslcContainerProcessOptionsInternal* GetInternalType(WslcProcessSettings* settings);

// CONTAINER DEFINITIONS
typedef struct WslcContainerOptionsInternal
{
    PCSTR image;       // Image name (repository:tag)
    PCSTR runtimeName; // Container runtime name (expected to allow DNS resolution between containers)
    PCSTR HostName;
    PCSTR DomainName;
    const WslcContainerPortMapping* ports;
    uint32_t portsCount;
    const WslcContainerVolume* volumes;
    uint32_t volumesCount;
    const WslcContainerNamedVolume* namedVolumes;
    uint32_t namedVolumesCount;
    const WslcContainerProcessOptionsInternal* initProcessOptions;
    WSLCContainerNetworkType networking;
    WslcContainerFlags containerFlags;
    PCSTR const* entrypoint;
    uint32_t entrypointCount;

} WslcContainerOptionsInternal;

static_assert(
    sizeof(WslcContainerOptionsInternal) == WSLC_CONTAINER_OPTIONS_SIZE, "WSLC_CONTAINER_OPTIONS_INTERNAL size mismatch");
static_assert(
    __alignof(WslcContainerOptionsInternal) == WSLC_CONTAINER_OPTIONS_ALIGNMENT,
    "WSLC_CONTAINER_OPTIONS_INTERNAL alignment mismatch");

static_assert(std::is_trivial_v<WslcContainerOptionsInternal>, "WSLC_CONTAINER_OPTIONS_INTERNAL must be trivial");

WslcContainerOptionsInternal* GetInternalType(WslcContainerSettings* settings);
const WslcContainerOptionsInternal* GetInternalType(const WslcContainerSettings* settings);

// Use to allocate the actual objects on the heap to keep it alive.
struct WslcSessionImpl
{
    wil::com_ptr<IWSLCSession> session;
    wil::com_ptr<ITerminationCallback> terminationCallback;
};

WslcSessionImpl* GetInternalType(WslcSession handle);

struct WslcContainerImpl
{
    wil::com_ptr<IWSLCContainer> container;
    WslcContainerProcessIOCallbackOptions ioCallbackOptions{};
    std::atomic<std::shared_ptr<IOCallback>> ioCallbacks;
};

WslcContainerImpl* GetInternalType(WslcContainer handle);

struct WslcProcessImpl
{
    wil::com_ptr<IWSLCProcess> process;
    std::shared_ptr<IOCallback> ioCallbacks;
};

WslcProcessImpl* GetInternalType(WslcProcess handle);

// Converts to the internal type and throws an error on null input.
template <typename T>
auto CheckAndGetInternalType(T* value)
{
    THROW_HR_IF_NULL(E_POINTER, value);
    return GetInternalType(value);
}

// Converts to the internal type and throws an error on null input.
template <typename T>
auto CheckAndGetInternalTypeUniquePointer(T* value)
{
    THROW_HR_IF_NULL(E_POINTER, value);
    return std::unique_ptr<std::remove_pointer_t<decltype(GetInternalType(value))>>{GetInternalType(value)};
}
