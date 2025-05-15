/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wdk.h

Abstract:

    This file contains various definitions that are needed for WSL to build.

--*/

#pragma once

#include <winternl.h>
#include <computedefs.h>
#include <winnt.h>
#include <windns.h>

#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#define STATUS_NO_SUCH_DEVICE ((NTSTATUS)0xC000000EL)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_DEVICE_NOT_CONNECTED ((NTSTATUS)0xC000009DL)
#define STATUS_DIRECTORY_NOT_EMPTY ((NTSTATUS)0xC0000101L)
#define STATUS_FILE_IS_A_DIRECTORY ((NTSTATUS)0xC00000BAL)
#define STATUS_NOT_A_DIRECTORY ((NTSTATUS)0xC0000103L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_REDIRECTOR_STARTED ((NTSTATUS)0xC00000FCL)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003AL)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xC00000E5L)
#define STATUS_CANCELLED ((NTSTATUS)0xC0000120L)
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#define STATUS_SHUTDOWN_IN_PROGRESS ((NTSTATUS)0xC00002FEL)

#define IOCTL_DISK_ARE_VOLUMES_READY CTL_CODE(IOCTL_DISK_BASE, 0x0087, METHOD_BUFFERED, FILE_READ_ACCESS)
#define DIRECTORY_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | 0xF)

#define JobObjectTimerVirtualizationInformation ((JOBOBJECTINFOCLASS)23) // TODO: Undocumented.
#define ThreadExplicitCaseSensitivity ((THREADINFOCLASS)43)              // TODO: Undocumented.
#define FileAttributeTagInformation ((FILE_INFORMATION_CLASS)35)
#define FileStatLxInformation ((FILE_INFORMATION_CLASS)70)
#define FileCaseSensitiveInformation ((FILE_INFORMATION_CLASS)71)
#define FileFullDirectoryInformation ((FILE_INFORMATION_CLASS)2)
#define FileStatInformation ((FILE_INFORMATION_CLASS)68)

#define IO_REPARSE_TAG_LX_SYMLINK (0xA000001D)

#define ARGUMENT_PRESENT(x) (((x) != NULL))
#define MAXULONG ULONG_MAX

// Note: These flags are documented but not in the SDK.
// See: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_stat_lx_information
#define LX_FILE_METADATA_HAS_UID 0x1
#define LX_FILE_METADATA_HAS_GID 0x2
#define LX_FILE_METADATA_HAS_MODE 0x4

// Note: This flag is documented but not in the SDK.
// See: https://learn.microsoft.com/en-us/windows/console/readconsoleinputex
#define CONSOLE_READ_NOWAIT 0x0002

// Note: This flag is already published in: https://github.com/microsoft/terminal/blob/main/dep/Console/condrv.h
#define IOCTL_CONDRV_GET_SERVER_PID CTL_CODE(FILE_DEVICE_CONSOLE, 8, METHOD_NEITHER, FILE_ANY_ACCESS)

#define VM_E_INVALID_STATE MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x1001) // TODO: Undocumented.

// {FC36C5C6-7A87-4841-A47A-1D352987055B}
DEFINE_GUID(VIRTIO_PLAN9_DEVICE_ID, 0xFC36C5C6, 0x7A87, 0x4841, 0xA4, 0x7A, 0x1D, 0x35, 0x29, 0x87, 0x05, 0x5B); // TODO: Undocumented.

// {a8679153-843f-467f-ad7e-f429328f7568}
DEFINE_GUID(FLEXIO_DEVICE_ID, 0xa8679153, 0x843f, 0x467f, 0xad, 0x7e, 0xf4, 0x29, 0x32, 0x8f, 0x75, 0x68); // TODO: Undocumented.

typedef struct _KEY_FLAGS_INFORMATION // TODO: Undocumented.
{
    ULONG UserFlags;
    ULONG KeyFlags; // LSB bit set --> Key is Volatile
    // second to LSB bit set --> Key is symlink

    ULONG ControlFlags; // combination of the above
} KEY_FLAGS_INFORMATION, *PKEY_FLAGS_INFORMATION;

typedef enum _EVENT_TYPE
{
    NotificationEvent,
    SynchronizationEvent
} EVENT_TYPE;

typedef IO_STATUS_BLOCK* PIO_STATUS_BLOCK;

#define RtlEqualLuid(L1, L2) (((L1)->LowPart == (L2)->LowPart) && ((L1)->HighPart == (L2)->HighPart))

typedef enum _FSINFOCLASS
{
    FileFsDeviceInformation = 4,
    FileIdBothDirectoryInformation = 37
} FS_INFORMATION_CLASS,
    *PFS_INFORMATION_CLASS;

typedef struct _FILE_FS_DEVICE_INFORMATION
{                                                           // ntddk nthal
    DEVICE_TYPE DeviceType;                                 // ntddk nthal
    ULONG Characteristics;                                  // ntddk nthal
} FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION; // ntddk nthal

typedef struct _REPARSE_DATA_BUFFER
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union
    {
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct
        {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    } DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

typedef struct _FILE_ATTRIBUTE_TAG_INFORMATION
{
    ULONG FileAttributes;
    ULONG ReparseTag;
} FILE_ATTRIBUTE_TAG_INFORMATION, *PFILE_ATTRIBUTE_TAG_INFORMATION;

#define REPARSE_DATA_BUFFER_HEADER_SIZE FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

typedef struct _FILE_GET_EA_INFORMATION
{
    ULONG NextEntryOffset;
    UCHAR EaNameLength;
    CHAR EaName[1];
} FILE_GET_EA_INFORMATION, *PFILE_GET_EA_INFORMATION;

typedef struct _FILE_FULL_EA_INFORMATION
{
    ULONG NextEntryOffset;
    UCHAR Flags;
    UCHAR EaNameLength;
    USHORT EaValueLength;
    CHAR EaName[1];
} FILE_FULL_EA_INFORMATION, *PFILE_FULL_EA_INFORMATION;

typedef struct _FILE_ID_BOTH_DIR_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    LARGE_INTEGER FileId;
    WCHAR FileName[1];
} FILE_ID_BOTH_DIR_INFORMATION, *PFILE_ID_BOTH_DIR_INFORMATION;

#ifdef __cplusplus
extern "C" {
#endif

BOOL WINAPI ReadConsoleInputExW(
    _In_ HANDLE hConsoleInput, _Out_writes_(nLength) PINPUT_RECORD lpBuffer, _In_ DWORD nLength, _Out_ LPDWORD lpNumberOfEventsRead, _In_ USHORT wFlags);

NTSTATUS WINAPI NtCancelIoFileEx(__in HANDLE FileHandle, __in_opt PIO_STATUS_BLOCK IoRequestToCancel, __out PIO_STATUS_BLOCK IoStatusBlock);

NTSTATUS
NtCreateNamedPipeFile(
    __out PHANDLE FileHandle,
    __in ULONG DesiredAccess,
    __in POBJECT_ATTRIBUTES ObjectAttributes,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __in ULONG ShareAccess,
    __in ULONG CreateDisposition,
    __in ULONG CreateOptions,
    __in ULONG NamedPipeType,
    __in ULONG ReadMode,
    __in ULONG CompletionMode,
    __in ULONG MaximumInstances,
    __in ULONG InboundQuota,
    __in ULONG OutboundQuota,
    __in_opt PLARGE_INTEGER DefaultTimeout);

NTSTATUS
NTSYSCALLAPI
NtFsControlFile(
    __in HANDLE FileHandle,
    __in_opt HANDLE Event,
    __in_opt PIO_APC_ROUTINE ApcRoutine,
    __in_opt PVOID ApcContext,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __in ULONG IoControlCode,
    __in_bcount_opt(InputBufferLength) PVOID InputBuffer,
    __in ULONG InputBufferLength,
    __out_bcount_opt(OutputBufferLength) PVOID OutputBuffer,
    __in ULONG OutputBufferLength);

NTSTATUS
NTSYSCALLAPI
NtQueryInformationByName(
    __in POBJECT_ATTRIBUTES ObjectAttributes,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __out_bcount(Length) PVOID FileInformation,
    __in ULONG Length,
    __in FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS
NTSYSCALLAPI
NtQueryVolumeInformationFile(
    __in HANDLE FileHandle, __out PIO_STATUS_BLOCK IoStatusBlock, __out_bcount(Length) PVOID FsInformation, __in ULONG Length, __in FS_INFORMATION_CLASS FsInformationClass);

NTSTATUS RtlDosPathNameToNtPathName_U_WithStatus(
    __in PCWSTR DosFileName, __out PUNICODE_STRING NtFileName, __deref_opt_out_opt PWSTR* FilePart, __reserved PVOID Reserved);

NTSTATUS
NTSYSAPI
RtlInitializeSidEx(__out_bcount(SECURITY_SID_SIZE(SubAuthorityCount)) PSID Sid, __in PSID_IDENTIFIER_AUTHORITY IdentifierAuthority, __in UCHAR SubAuthorityCount, ...);

NTSTATUS
NTSYSAPI
ZwCreateDirectoryObject(__out PHANDLE DirectoryHandle, __in ACCESS_MASK DesiredAccess, __in POBJECT_ATTRIBUTES ObjectAttributes);

NTSTATUS
WINAPI
NtOpenDirectoryObject(__out PHANDLE DirectoryHandle, __in ACCESS_MASK DesiredAccess, __in POBJECT_ATTRIBUTES ObjectAttributes);

NTSTATUS
NTSYSCALLAPI
NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS NTSYSCALLAPI NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);

NTSTATUS
ZwQueryEaFile(
    __in HANDLE FileHandle,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __out_bcount(Length) PVOID Buffer,
    __in ULONG Length,
    __in BOOLEAN ReturnSingleEntry,
    __in_bcount_opt(EaListLength) PVOID EaList,
    __in ULONG EaListLength,
    __in_opt PULONG EaIndex,
    __in BOOLEAN RestartScan);

NTSTATUS
NTSYSAPI
ZwCreateEvent(__out PHANDLE EventHandle, __in ACCESS_MASK DesiredAccess, __in_opt POBJECT_ATTRIBUTES ObjectAttributes, __in EVENT_TYPE EventType, __in BOOLEAN InitialState);

NTSTATUS
NtReadFile(
    __in HANDLE FileHandle,
    __in_opt HANDLE Event,
    __in_opt PIO_APC_ROUTINE ApcRoutine,
    __in_opt PVOID ApcContext,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __out PVOID Buffer,
    __in ULONG Length,
    __in_opt PLARGE_INTEGER ByteOffset,
    __in_opt PULONG Key);

NTSTATUS
NTSYSCALLAPI
NtWriteFile(
    __in HANDLE FileHandle,
    __in_opt HANDLE Event,
    __in_opt PIO_APC_ROUTINE ApcRoutine,
    __in_opt PVOID ApcContext,
    __out PIO_STATUS_BLOCK IoStatusBlock,
    __in PVOID Buffer,
    __in ULONG Length,
    __in_opt PLARGE_INTEGER ByteOffset,
    __in_opt PULONG Key);

NTSTATUS
NTSYSCALLAPI
NtQueryDirectoryFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    PUNICODE_STRING FileName,
    BOOLEAN RestartScan);

NTSTATUS ZwSetEaFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length);

// These extended attributes are defined in ntioapi_x.h in the OS repo and are not present in the SDK.
#define LX_FILE_METADATA_UID_EA_NAME "$LXUID"
#define LX_FILE_METADATA_GID_EA_NAME "$LXGID"
#define LX_FILE_METADATA_MODE_EA_NAME "$LXMOD"
#define LX_FILE_METADATA_DEVICE_ID_EA_NAME "$LXDEV"

#define SYMLINK_FLAG_RELATIVE 0x00000001 // If set then this is a relative symlink.

#ifdef _AMD64_

typedef struct _HV_X64_HYPERVISOR_HARDWARE_FEATURES
{
    //
    // Eax
    //
    UINT32 ApicOverlayAssistInUse : 1;
    UINT32 MsrBitmapsInUse : 1;
    UINT32 ArchitecturalPerformanceCountersInUse : 1;
    UINT32 SecondLevelAddressTranslationInUse : 1;
    UINT32 DmaRemappingInUse : 1;
    UINT32 InterruptRemappingInUse : 1;
    UINT32 MemoryPatrolScrubberPresent : 1;
    UINT32 DmaProtectionInUse : 1;
    UINT32 HpetRequested : 1;
    UINT32 SyntheticTimersVolatile : 1;
    UINT32 HypervisorLevel : 4;
    UINT32 PhysicalDestinationModeRequired : 1;
    UINT32 UseVmfuncForAliasMapSwitch : 1;
    UINT32 HvRegisterForMemoryZeroingSupported : 1;
    UINT32 UnrestrictedGuestSupported : 1;
    UINT32 RdtAFeaturesSupported : 1;
    UINT32 RdtMFeaturesSupported : 1;
    UINT32 ChildPerfmonPmuSupported : 1;
    UINT32 ChildPerfmonLbrSupported : 1;
    UINT32 ChildPerfmonIptSupported : 1;
    UINT32 ApicEmulationSupported : 1;
    UINT32 ChildX2ApicRecommended : 1;
    UINT32 HardwareWatchdogReserved : 1;
    UINT32 DeviceAccessTrackingSupported : 1;
    UINT32 Reserved : 5;

    //
    // Ebx
    //
    UINT32 DeviceDomainInputWidth : 8;
    UINT32 ReservedEbx : 24;

    //
    // Ecx
    //
    UINT32 ReservedEcx;

    //
    // Edx
    //
    UINT32 ReservedEdx;

} HV_X64_HYPERVISOR_HARDWARE_FEATURES, *PHV_X64_HYPERVISOR_HARDWARE_FEATURES;

#define HvCpuIdFunctionMsHvHardwareFeatures 0x40000006

#endif

typedef enum _KEY_INFORMATION_CLASS
{
    KeyBasicInformation,
    KeyNodeInformation,
    KeyFullInformation,
    KeyNameInformation,
    KeyCachedInformation,
    KeyFlagsInformation,
    KeyVirtualizationInformation,
    KeyHandleTagsInformation,
    KeyTrustInformation,
    KeyLayerInformation,
    MaxKeyInfoClass
} KEY_INFORMATION_CLASS;

typedef struct _KEY_NAME_INFORMATION
{
    ULONG NameLength;
    WCHAR Name[1];
} KEY_NAME_INFORMATION, *PKEY_NAME_INFORMATION;

NTSYSAPI NTSTATUS ZwQueryKey(HANDLE KeyHandle, KEY_INFORMATION_CLASS KeyInformationClass, PVOID KeyInformation, ULONG Length, PULONG ResultLength);

STDAPI
GetVmWorkerProcess(_In_ REFGUID VirtualMachineId, _In_ REFIID ObjectIid, _Outptr_opt_ IUnknown** Object);

HRESULT
WINAPI
HdvProxyDeviceHost(HCS_SYSTEM ComputeSystem, _In_ PVOID DeviceHost_IUnknown, DWORD TargetProcessId, _Out_ UINT64* IpcSectionHandle);

#ifdef __cplusplus
}

// List of methods that shouldn't be used because they aren't available on older builds that WSL supports

template <typename T>
T BreakBuild()
{
    static_assert(false, "Do not use GetTempPath2W(). (Not available on vb)");
    return T{};
}

#define GetTempPath2W(...) (BreakBuild<bool>())

#endif
