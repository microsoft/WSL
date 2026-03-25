/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVhdVolume.h

Abstract:

    Internal implementation for a VHD-backed volume.

--*/

#pragma once

#include "WSLAVolumeMetadata.h"
#include "wslaservice.h"
#include <filesystem>
#include <memory>
#include <string>

namespace wsl::windows::common::docker_schema {
struct Volume;
}

namespace wsl::windows::service::wsla {

class WSLAVirtualMachine;
class DockerHTTPClient;

class WSLAVhdVolumeImpl
{
public:
    NON_COPYABLE(WSLAVhdVolumeImpl);
    NON_MOVABLE(WSLAVhdVolumeImpl);

    WSLAVhdVolumeImpl(
        std::string&& Name,
        std::filesystem::path&& HostPath,
        ULONGLONG SizeBytes,
        ULONG Lun,
        std::string&& VirtualMachinePath,
        WSLAVirtualMachine& VirtualMachine,
        DockerHTTPClient& DockerClient);

    ~WSLAVhdVolumeImpl();

    static std::unique_ptr<WSLAVhdVolumeImpl> Create(
        _In_ const WSLAVolumeOptions& Options,
        _In_ const std::filesystem::path& StoragePath,
        _In_ WSLAVirtualMachine& VirtualMachine,
        _In_ DockerHTTPClient& DockerClient);

    static std::unique_ptr<WSLAVhdVolumeImpl> Open(
        _In_ const wsl::windows::common::docker_schema::Volume& Volume, _In_ WSLAVirtualMachine& VirtualMachine, _In_ DockerHTTPClient& DockerClient);

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
    WSLAVirtualMachine& m_virtualMachine;
    DockerHTTPClient& m_dockerClient;
    bool m_attached{true};
};

} // namespace wsl::windows::service::wsla
