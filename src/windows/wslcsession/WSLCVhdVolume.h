/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVhdVolume.h

Abstract:

    Internal implementation for a VHD-backed volume.

--*/

#pragma once

#include "WSLCVolumeMetadata.h"
#include "wslc.h"
#include <filesystem>
#include <memory>
#include <string>

namespace wsl::windows::common::docker_schema {
struct Volume;
}

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;
class DockerHTTPClient;

class WSLCVhdVolumeImpl
{
public:
    NON_COPYABLE(WSLCVhdVolumeImpl);
    NON_MOVABLE(WSLCVhdVolumeImpl);

    WSLCVhdVolumeImpl(
        std::string&& Name,
        std::filesystem::path&& HostPath,
        ULONGLONG SizeBytes,
        ULONG Lun,
        std::string&& VirtualMachinePath,
        WSLCVirtualMachine& VirtualMachine,
        DockerHTTPClient& DockerClient);

    ~WSLCVhdVolumeImpl();

    static std::unique_ptr<WSLCVhdVolumeImpl> Create(
        _In_ const WSLCVolumeOptions& Options,
        _In_ const std::filesystem::path& StoragePath,
        _In_ WSLCVirtualMachine& VirtualMachine,
        _In_ DockerHTTPClient& DockerClient);

    static std::unique_ptr<WSLCVhdVolumeImpl> Open(
        _In_ const wsl::windows::common::docker_schema::Volume& Volume, _In_ WSLCVirtualMachine& VirtualMachine, _In_ DockerHTTPClient& DockerClient);

    void Delete();
    std::string Inspect() const;

    const std::string& Name() const noexcept
    {
        return m_name;
    }
    const std::string& VirtualMachinePath() const noexcept
    {
        return m_virtualMachinePath;
    }

private:
    void Detach();

    std::string m_name;
    std::filesystem::path m_hostPath;
    std::string m_virtualMachinePath;
    ULONGLONG m_sizeBytes{};
    ULONG m_lun{};
    WSLCVirtualMachine& m_virtualMachine;
    DockerHTTPClient& m_dockerClient;
    bool m_attached{true};
};

} // namespace wsl::windows::service::wslc
