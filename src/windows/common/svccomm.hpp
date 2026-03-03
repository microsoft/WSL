/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccomm.hpp

Abstract:

    This file contains function declarations for the SvcComm helper class.

--*/

#pragma once

#include <vector>
#include <memory>
#include "helpers.hpp"
#include "SocketChannel.h"

namespace wsl::windows::common {

void RelayStandardInput(HANDLE ConsoleHandle, HANDLE OutputHandle, const std::shared_ptr<wsl::shared::SocketChannel>& ControlChannel, HANDLE ExitEvent, ConsoleState* Io);

class SvcComm
{
public:
    struct MountResult
    {
        int Result = -1;
        int Step = 0;
        wil::unique_cotaskmem_string MountName;
    };

    SvcComm();
    ~SvcComm();

    void ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags) const;

    void CreateInstance(_In_opt_ LPCGUID DistroGuid = nullptr, _In_ ULONG Flags = LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE);

    HRESULT
    CreateInstanceNoThrow(_In_opt_ LPCGUID DistroGuid = nullptr, _In_ ULONG Flags = LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE, LXSS_ERROR_INFO* Error = nullptr) const;

    std::vector<LXSS_ENUMERATE_INFO> EnumerateDistributions() const;

    HRESULT
    ExportDistribution(_In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ ULONG Flags = 0) const;

    void GetDistributionConfiguration(
        _In_opt_ LPCGUID DistroGuid,
        _Out_ LPWSTR* Name,
        _Out_ ULONG* Version,
        _Out_ ULONG* DefaultUid,
        _Out_ ULONG* DefaultEnvironmentCount,
        _Out_ LPSTR** DefaultEnvironment,
        _Out_ ULONG* Flags) const;

    DWORD
    LaunchProcess(
        _In_opt_ LPCGUID DistroGuid,
        _In_opt_ LPCWSTR Filename,
        _In_ int Argc,
        _In_reads_(Argc) LPCWSTR Argv[],
        _In_ ULONG LaunchFlags = 0,
        _In_opt_ PCWSTR Username = nullptr,
        _In_opt_ PCWSTR CurrentWorkingDirectory = nullptr,
        _In_ DWORD Timeout = INFINITE) const;

    GUID GetDefaultDistribution() const;

    ULONG
    GetDistributionFlags(_In_opt_ LPCGUID DistroGuid = nullptr) const;

    GUID GetDistributionId(_In_ LPCWSTR Name, _In_ ULONG Flags = 0) const;

    GUID ImportDistributionInplace(_In_ LPCWSTR Name, _In_ LPCWSTR VhdPath) const;

    MountResult MountDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags, _In_ ULONG PartitionIndex, _In_opt_ LPCWSTR Name, _In_opt_ LPCWSTR Type, _In_opt_ LPCWSTR Options) const;

    std::pair<GUID, wil::unique_cotaskmem_string> RegisterDistribution(
        _In_ LPCWSTR Name,
        _In_ ULONG Version,
        _In_ HANDLE FileHandle,
        _In_ LPCWSTR TargetDirectory,
        _In_ ULONG Flags,
        _In_ std::optional<uint64_t> VhdSize = std::nullopt,
        _In_opt_ LPCWSTR PackageFamilyName = nullptr) const;

    HRESULT
    ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ ULONG64 NewSize) const;

    void SetDefaultDistribution(_In_ LPCGUID DistroGuid) const;

    HRESULT
    SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOL Sparse, _In_ BOOL AllowUnsafe) const;

    HRESULT
    SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version) const;

    HRESULT
    AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags) const;

    std::pair<int, int> DetachDisk(_In_opt_ LPCWSTR Disk) const;

    void Shutdown(_In_ bool Force) const;

    void TerminateInstance(_In_opt_ LPCGUID DistroGuid = nullptr) const;

    void UnregisterDistribution(_In_ LPCGUID DistroGuid) const;

    void MoveDistribution(_In_ const GUID& DistroGuid, _In_ LPCWSTR Location) const;

private:
    wil::com_ptr<ILxssUserSession> m_userSession;
};
} // namespace wsl::windows::common
