/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCGuestVolume.h

Abstract:

    Volume implementation that delegates storage to docker's built-in "local"
    volume driver. The volume lives on the session storage VHD at
    /var/lib/docker/volumes/<name>/_data inside the guest VM. No host-side
    artifacts (no extra VHD file, no disk attach).

--*/

#pragma once

#include "IWSLCVolume.h"
#include "WSLCVolumeMetadata.h"
#include "wslc.h"
#include <map>
#include <memory>
#include <string>

namespace wsl::windows::common::docker_schema {
struct Volume;
}

namespace wsl::windows::service::wslc {

class DockerHTTPClient;

class WSLCGuestVolumeImpl : public IWSLCVolume
{
public:
    NON_COPYABLE(WSLCGuestVolumeImpl);
    NON_MOVABLE(WSLCGuestVolumeImpl);

    WSLCGuestVolumeImpl(
        std::string&& Name,
        std::string&& CreatedAt,
        std::map<std::string, std::string>&& DriverOpts,
        std::map<std::string, std::string>&& Labels,
        DockerHTTPClient& DockerClient);

    ~WSLCGuestVolumeImpl() = default;

    static std::unique_ptr<WSLCGuestVolumeImpl> Create(
        _In_opt_ LPCSTR Name,
        _In_ std::map<std::string, std::string>&& DriverOpts,
        _In_ std::map<std::string, std::string>&& Labels,
        _In_ DockerHTTPClient& DockerClient);

    static std::unique_ptr<WSLCGuestVolumeImpl> Open(
        _In_ const wsl::windows::common::docker_schema::Volume& Volume, _In_ DockerHTTPClient& DockerClient);

    // IWSLCVolume
    const std::string& Name() const noexcept override
    {
        return m_name;
    }
    const char* Driver() const noexcept override
    {
        return WSLCGuestVolumeDriver;
    }
    void Delete() override;
    std::string Inspect() const override;
    WSLCVolumeInformation GetVolumeInformation() const override;

private:
    std::string m_name;
    std::string m_createdAt;
    std::map<std::string, std::string> m_driverOpts;
    std::map<std::string, std::string> m_labels;
    DockerHTTPClient& m_dockerClient;
};

} // namespace wsl::windows::service::wslc
