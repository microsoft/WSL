/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVhdVolume.h

Abstract:

    Internal implementation for a VHD-backed volume.

--*/

#pragma once

#include "wslaservice.h"
#include <filesystem>
#include <memory>
#include <string>

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;

class WSLAVhdVolumeImpl
{
public:
    NON_COPYABLE(WSLAVhdVolumeImpl);
    NON_MOVABLE(WSLAVhdVolumeImpl);

    WSLAVhdVolumeImpl(
        std::wstring&& Name,
        std::wstring&& Type,
        std::filesystem::path&& HostPath,
        ULONGLONG SizeBytes,
        ULONG Lun,
        std::string&& VirtualMachinePath,
        WSLAVirtualMachine* VirtualMachine);

    ~WSLAVhdVolumeImpl();

    static std::unique_ptr<WSLAVhdVolumeImpl> Create(
        _In_ const WSLA_VOLUME_OPTIONS& Options,
        _In_ const std::filesystem::path& StoragePath,
        _In_ WSLAVirtualMachine& VirtualMachine);

    void Delete();

    const std::wstring& Name() const noexcept { return m_name; }
    const std::string& VirtualMachinePath() const noexcept { return m_virtualMachinePath; }

private:
    void Detach();

    std::wstring m_name;
    std::wstring m_type;
    std::filesystem::path m_hostPath;
    std::string m_virtualMachinePath;
    ULONGLONG m_sizeBytes{};
    ULONG m_lun{};
    WSLAVirtualMachine* m_virtualMachine{};
};

} // namespace wsl::windows::service::wsla
