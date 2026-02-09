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


static_assert(sizeof(WSLC_SESSION_OPTIONS_INTERNAL) == WSLC_SESSION_OPTIONS_SIZE, "WSLC_SESSION_OPTIONS_INTERNAL size mismatch");
static_assert(__alignof(WSLC_SESSION_OPTIONS_INTERNAL) == WSLC_SESSION_OPTIONS_ALIGNMENT, "WSLC_SESSION_OPTIONS_INTERNAL alignment mismatch");

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


static_assert(sizeof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL) == WSLC_CONTAINER_PROCESS_OPTIONS_SIZE, "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL must be 48 bytes");
static_assert(__alignof(WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL) == WSLC_CONTAINER_PROCESS_OPTIONS_ALIGNMENT, "WSLC_CONTAINER_PROCESS_OPTIONS_INTERNAL must be 8-byte aligned");


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
static_assert(sizeof(WSLC_CONTAINER_OPTIONS_INTERNAL) == WSLC_CONTAINER_OPTIONS_SIZE, "WSLC_CONTAINER_OPTIONS_INTERNAL must be 80 bytes");
static_assert(__alignof(WSLC_CONTAINER_OPTIONS_INTERNAL) == WSLC_CONTAINER_OPTIONS_ALIGNMENT,"WSLC_CONTAINER_OPTIONS_INTERNAL must be 8-byte aligned");



// Use to allocate the actual objects on the heap to keep it alive.
struct WslcSessionImpl
{
    wil::com_ptr<IWSLASessionManager> sessionManager;
};

struct WslcContainerImpl
{
    wil::com_ptr<IWSLAContainer> container;
};

struct WslcProcessImpl
{
    wil::com_ptr<IWSLAProcess> process;
};

