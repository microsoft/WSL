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
#include "wslaservice.h"
#include <stdint.h>
#include <wil/com.h> // COM helpers
// #include <wil/resource.h> // handle wrappers
// #include <wil/result.h>   // error handling

// SESSION DEFINITIONS
typedef struct WSLC_SESSION_OPTIONS_INTERNAL
{
    PCWSTR displayName;
    PCWSTR storagePath;

    uint32_t cpuCount;
    uint32_t memoryMb;
    uint32_t timeoutMS;

    WslcVhdRequirements vhdRequirements;
    WslcSessionFlags flags;
    WslcSessionTerminationCallback terminationCallback;
    PVOID terminationCallbackContext;
} WSLC_SESSION_OPTIONS_INTERNAL;

static_assert(sizeof(WSLC_SESSION_OPTIONS_INTERNAL) == WSLC_SESSION_OPTIONS_SIZE, "WSLC_SESSION_OPTIONS_INTERNAL size mismatch");

static_assert(
    __alignof(WSLC_SESSION_OPTIONS_INTERNAL) == WSLC_SESSION_OPTIONS_ALIGNMENT,
    "WSLC_SESSION_OPTIONS_INTERNAL alignment mismatch");

WSLC_SESSION_OPTIONS_INTERNAL* GetInternalType(WslcSessionSettings* settings);

// PROCESS DEFINITIONS
typedef struct WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL
{
    PCSTR executable; // path to executable inside container
    PCSTR const* commandLine;
    UINT32 commandLineCount;
    PCSTR const* environment;
    UINT32 environmentCount;
    PCSTR currentDirectory;
} WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL;

static_assert(
    sizeof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL) == WSLC_CONTAINER_PROCESS_OPTIONS_SIZE,
    "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL must be 48 bytes");
static_assert(
    __alignof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL) == WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT,
    "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL must be 8-byte aligned");

WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* GetInternalType(WslcProcessSettings* settings);

// CONTAINER DEFINITIONS
typedef struct WSLC_CONTAINER_OPTIONS_INTERNAL
{
    PCSTR image;       // Image name (repository:tag)
    PCSTR runtimeName; // Container runtime name (expected to allow DNS resolution between containers)
    PCSTR HostName;
    PCSTR DomainName;
    const WslcContainerPortMapping* ports;
    UINT32 portsCount;
    const WslcContainerVolume* volumes;
    UINT32 volumesCount;
    const WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* initProcessOptions;
    WslcContainerNetworkingMode networking;
    WslcContainerFlags containerFlags;

} WSLC_CONTAINER_OPTIONS_INTERNAL;

static_assert(
    sizeof(WSLC_CONTAINER_OPTIONS_INTERNAL) == WSLC_CONTAINER_OPTIONS_SIZE, "WSLC_CONTAINER_OPTIONS_INTERNAL must be 80 bytes");
static_assert(
    __alignof(WSLC_CONTAINER_OPTIONS_INTERNAL) == WSLC_CONTAINER_OPTIONS_ALIGNMENT,
    "WSLC_CONTAINER_OPTIONS_INTERNAL must be 8-byte aligned");

WSLC_CONTAINER_OPTIONS_INTERNAL* GetInternalType(WslcContainerSettings* settings);

// Use to allocate the actual objects on the heap to keep it alive.
struct WslcSessionImpl
{
    wil::com_ptr<IWSLASession> session;
    wil::com_ptr<ITerminationCallback> terminationCallback;
};

WslcSessionImpl* GetInternalType(WslcSession handle);

struct WslcContainerImpl
{
    wil::com_ptr<IWSLAContainer> container;
};

WslcContainerImpl* GetInternalType(WslcContainer handle);

struct WslcProcessImpl
{
    wil::com_ptr<IWSLAProcess> process;
};

WslcProcessImpl* GetInternalType(WslcProcess handle);

// Returns an error on null input then converts to a local named `internalType`.
#define WSLC_GET_INTERNAL_TYPE_NAMED(_input_,_name_) \
    RETURN_HR_IF_NULL(E_POINTER, _input_); \
    auto _name_ = GetInternalType(_input_) \

#define WSLC_GET_INTERNAL_TYPE(_input_) WSLC_GET_INTERNAL_TYPE_NAMED(_input_, internalType)

// Returns an error on null input then converts to a unique_ptr local named `internalType`.
// Use for Release functions to clean up the implementation object on return.
#define WSLC_GET_INTERNAL_TYPE_FOR_RELEASE(_input_) \
    RETURN_HR_IF_NULL(E_POINTER, _input_); \
    std::unique_ptr<std::remove_pointer_t<decltype(GetInternalType(_input_))>> internalType{ GetInternalType(_input_) }\
