/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wsla.h

Abstract:

    This file contains the public WSLA api definitions.

--*/
#pragma once

#include <windows.h>

EXTERN_C_START

// Wsla Install

STDAPI WslaCanRun(_Out_ BOOL* canRun);

typedef struct WSLA_VERSION
{
    UINT32 major;
    UINT32 minor;
    UINT32 revision;
} WSLA_VERSION;

STDAPI WslaGetVersion(_Out_ WSLA_VERSION* version);

typedef enum WSLA_INSTALL_COMPONENT
{
    WSLA_INSTALL_COMPONENT_NONE = 0,
    WSLA_INSTALL_COMPONENT_VMPOC = 1,
    WSLA_INSTALL_COMPONENT_WSL_OC = 2,
    WSLA_INSTALL_COMPONENT_WSL_PACKAGE = 4,
} WSLA_INSTALL_COMPONENT;

typedef __callback VOID(CALLBACK WslaInstallCallback)(_In_ WSLA_INSTALL_COMPONENT component, _In_ UINT32 progress, _In_ UINT32 total, _In_opt_ PVOID context);

STDAPI WslaInstallWithDependencies(_In_opt_ __callback WslaInstallCallback progressCallback, _In_opt_ PVOID context);

// Session

typedef enum WSLA_SESSION_TERMINATION_REASON
{
    WSLA_SESSION_TERMINATION_REASON_UNKNOWN = 0,
    WSLA_SESSION_TERMINATION_REASON_SHUTDOWN = 1,
    WSLA_SESSION_TERMINATION_REASON_CRASHED = 2,
} WSLA_SESSION_TERMINATION_REASON;

typedef __callback VOID(CALLBACK WslaSessionTerminationCallback)(_In_ WSLA_SESSION_TERMINATION_REASON reason, _In_opt_ PVOID context);

typedef struct WSLA_CREATE_SESSION_OPTIONS
{
    PCWSTR displayName;
    PCWSTR storagePath;
    WslaSessionTerminationCallback terminationCallback;
    PVOID terminationCallbackContext;
} WSLA_CREATE_SESSION_OPTIONS;

DECLARE_HANDLE(WslaSession);

STDAPI WslaCreateSession(_In_ const WSLA_CREATE_SESSION_OPTIONS* settings, _Out_ WslaSession* session);

STDAPI WslaReleaseSession(_In_ WslaSession session);

// Container image

typedef __callback VOID(CALLBACK WslaContainerImageProgressCallback)(_In_ UINT32 progress, _In_ UINT32 total, _In_opt_ PVOID context);

typedef struct WLSA_PULL_CONTAINER_IMAGE_OPTIONS
{
    PCSTR uri; // e.g. "my.registry.io/hello-world:latest" or just "hello-world:latest" which will default to docker
    WslaContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;

    // TODO: think about authentication
    PCSTR account;
    PCSTR password;
} WLSA_PULL_CONTAINER_IMAGE_OPTIONS;

STDAPI WslaPullContainerImage(_In_ WslaSession session, _In_ const WLSA_PULL_CONTAINER_IMAGE_OPTIONS* options);

typedef struct WLSA_IMPORT_CONTAINER_IMAGE_OPTIONS
{
    PCWSTR imagePath;
    WslaContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
} WLSA_IMPORT_CONTAINER_IMAGE_OPTIONS;

STDAPI WslaImportContainerImage(_In_ WslaSession session, _In_ const WLSA_PULL_CONTAINER_IMAGE_OPTIONS* options);

typedef struct WSLA_CONTAINER_IMAGE_INFO
{
    PCSTR repository;
    PCSTR tag;
    UINT8 sha256[32];
} WSLA_CONTAINER_IMAGE_INFO;

STDAPI WslaListContainerImages(_In_ WslaSession sesssion, _Inout_ WSLA_CONTAINER_IMAGE_INFO* images, _Inout_ UINT32* count);

STDAPI WslaDeleteContainerImage(_In_ WslaSession session, _In_ PCSTR imageName);

// Container

typedef struct WSLA_CONTAINER_PORT_MAPPING
{
    UINT16 windowsPort;
    UINT16 containerPort;

    // TODO: Port mapping type? Host/Bridge, etc
} WSLA_CONTAINER_PORT_MAPPING;

typedef struct WSLA_CONTAINER_VOLUME
{
    PCWSTR windowsPath;
    PCSTR containerPath;
} WSLA_CONTAINER_VOLUME;

typedef struct WSLA_CONTAINER_GPU_OPTIONS
{
    BOOL enable;
    PCSTR gpuDevices;
} WSLA_CONTAINER_GPU_OPTIONS;

typedef struct WSLA_CONTAINER_PROCESS_OPTIONS
{
    PCSTR executable; // Full path to executable inside container
    PCSTR* commandLine;
    UINT32 commandLineCount;
    PCSTR* environment;
    UINT32 environmentCount;
    PCSTR currentDirectory;
} WSLA_CONTAINER_PROCESS_OPTIONS;

typedef struct WSLA_CONTAINER_OPTIONS
{
    PCSTR image; // Image name (repository:tag)
    PCSTR name;  // Container runtime name (expected to allow DNS resolution between containers)
    const WSLA_CONTAINER_PORT_MAPPING* ports;
    UINT32 portsCount;
    const WSLA_CONTAINER_VOLUME* volumes;
    UINT32 volumesCount;
    const WSLA_CONTAINER_GPU_OPTIONS* gpuOptions;
    const WSLA_CONTAINER_PROCESS_OPTIONS* initProcessOptions;
} WSLA_CONTAINER_OPTIONS;

typedef struct WSLA_CONTAINER_PROCESS
{
    UINT32 pid;
    HANDLE exitEvent;
    HANDLE stdIn;
    HANDLE stdOut;
    HANDLE stdErr;
} WSLA_CONTAINER_PROCESS;

DECLARE_HANDLE(WslaRuntimeContainer);

STDAPI WslaCreateNewContainer(
    _In_ WslaSession session, _In_ const WSLA_CONTAINER_OPTIONS* options, _Out_ WslaRuntimeContainer* container, _Out_ WSLA_CONTAINER_PROCESS* initProcess);

STDAPI WslaStartContainer(_In_ WslaRuntimeContainer container);

STDAPI WslaStopContainer(_In_ WslaRuntimeContainer container);

STDAPI WslaDeleteContainer(_In_ WslaRuntimeContainer container);

STDAPI WslaRestartContainer(_In_ WslaRuntimeContainer container);

typedef enum WSLA_CONTAINER_STATE
{
    WSLA_CONTAINER_STATE_INVALID = 0,
    WSLA_CONTAINER_STATE_CREATED = 1,
    WSLA_CONTAINER_STATE_RUNNING = 2,
    WSLA_CONTAINER_STATE_EXITED = 3,
    WSLA_CONTAINER_STATE_FAILED = 4,
} WSLA_CONTAINER_STATE;

STDAPI WslaGetContainerState(_In_ WslaRuntimeContainer container, _Out_ WSLA_CONTAINER_STATE* state);

// Container Process

STDAPI WslaCreateContainerProcess(_In_ WslaRuntimeContainer container, _In_ const WSLA_CONTAINER_PROCESS_OPTIONS* options, _Out_ WSLA_CONTAINER_PROCESS* process);

typedef enum WSLA_CONTAINER_PROCESS_STATE
{
    WSLA_CONTAINER_PROCESS_STATE_UNKNOWN = 0,
    WSLA_CONTAINER_PROCESS_STATE_RUNNING = 1,
    WSLA_CONTAINER_PROCESS_STATE_EXITED = 2,
    WSLA_CONTAINER_PROCESS_STATE_SIGNALED = 3
} WSLA_CONTAINER_PROCESS_STATE;

typedef struct WSLA_CONTAINER_PROCESS_RESULT
{
    WSLA_CONTAINER_PROCESS_STATE state;
    INT32 exitCode;
} WSLA_CONTAINER_PROCESS_RESULT;

STDAPI WslaGetContainerProcessResult(_In_ const WSLA_CONTAINER_PROCESS* process, _Out_ WSLA_CONTAINER_PROCESS_RESULT* result);

STDAPI WslaSignalContainerProcess(_In_ WSLA_CONTAINER_PROCESS* process, _In_ INT32 signal);

// Storage

typedef enum WSLA_CREATE_VHD_TYPE
{
    WSLA_CREATE_VHD_TYPE_FIXED = 0,
    WSLA_CREATE_VHD_TYPE_DYNAMIC = 1,
} WSLA_CREATE_VHD_TYPE;

typedef struct WSLA_CREATE_VHD_OPTIONS
{
    PCWSTR vhdPath;
    WSLA_CREATE_VHD_TYPE vhdType;
    UINT64 maxSize; // Maximum size in bytes.
} WSLA_CREATE_VHD_OPTIONS;

STDAPI WslaCreateVhd(_In_ const WSLA_CREATE_VHD_OPTIONS* options);

EXTERN_C_END