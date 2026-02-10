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
#include <wil/com.h>      // COM helpers
//#include <wil/resource.h> // handle wrappers
//#include <wil/result.h>   // error handling

// SESSION DEFINITIONS
typedef struct WSLC_SESSION_OPTIONS_INTERNAL
{
    PCWSTR displayName;
    PCWSTR storagePath;

    uint32_t cpuCount;
    uint64_t memoryMb;
    uint32_t timeoutMS;

    WSLC_VHD_REQUIREMENTS vhdRequirements;
    WSLC_SESSION_FLAGS flags;
    WslcSessionTerminationCallback terminationCallback;
    PVOID terminationCallbackContext;
} WSLC_SESSION_OPTIONS_INTERNAL;

typedef char WSLC_SESSION_OPTIONS_INTERNAL_SIZE_ASSERT[sizeof(WSLC_SESSION_OPTIONS_INTERNAL)];//size = 80
typedef char WSLC_SESSION_OPTIONS_INTERNAL_ALIGNMENT_ASSERT[__alignof(WSLC_SESSION_OPTIONS_INTERNAL)];//alignment = 8

WSLC_SESSION_OPTIONS_INTERNAL* GetInternalType(WslcSessionSettings* settings);

// PROCESS DEFINITIONS
typedef struct WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL
{
    PCSTR executable; // path to executable inside container
    PCSTR* commandLine;
    UINT32 commandLineCount;
    PCSTR* environment;
    UINT32 environmentCount;
    PCSTR currentDirectory;
} WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL;

typedef char WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL_SIZE_ASSERT[sizeof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL)]; // size = 48
typedef char WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL_ALIGNMENT_ASSERT[__alignof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL)]; // alignment = 8

WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* GetInternalType(WslcProcessSettings* settings);

// CONTAINER DEFINITIONS
typedef struct WSLC_CONTAINER_OPTIONS_INTERNAL
{
    PCSTR image;       // Image name (repository:tag)
    PCSTR runtimeName; // Container runtime name (expected to allow DNS resolution between containers)
    PCSTR HostName;
    PCSTR DomainName;
    const WSLC_CONTAINER_PORT_MAPPING* ports;
    UINT32 portsCount;
    const WSLC_CONTAINER_VOLUME* volumes;
    UINT32 volumesCount;
    const WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL* initProcessOptions;
    WSLC_ContainerNetworkingMode networking;
    WSLC_CONTAINER_FLAGS containerFlags;

} WSLC_CONTAINER_OPTIONS_INTERNAL;
typedef char WSLC_CONTAINER_OPTIONS_INTERNAL_SIZE_ASSERT[sizeof(WSLC_CONTAINER_OPTIONS_INTERNAL)];         // size = 80
typedef char WSLC_CONTAINER_OPTIONS_INTERNAL_ALIGNMENT_ASSERT[__alignof(WSLC_CONTAINER_OPTIONS_INTERNAL)]; // alignment = 8

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
#define WSLC_GET_INTERNAL_TYPE(_input_) \
    RETURN_HR_IF_NULL(E_POINTER, _input_); \
    auto internalType = GetInternalType(_input_) \

// Returns an error on null input then converts to a unique_ptr local named `internalType`.
// Use for Release functions to clean up the implementation object on return.
#define WSLC_GET_INTERNAL_TYPE_FOR_RELEASE(_input_) \
    RETURN_HR_IF_NULL(E_POINTER, _input_); \
    std::unique_ptr<std::remove_pointer_t<decltype(GetInternalType(_input_))>> internalType{ GetInternalType(_input_) }\
