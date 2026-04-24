/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumes.h

Abstract:

    Contains the definition for WSLCVolumes.

--*/

#pragma once

#include "IWSLCVolume.h"
#include "WSLCVolumeMetadata.h"
#include "DockerHTTPClient.h"
#include "DockerEventTracker.h"
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

class WSLCVolumes
{
public:
    NON_COPYABLE(WSLCVolumes);
    NON_MOVABLE(WSLCVolumes);

    WSLCVolumes(DockerHTTPClient& dockerClient, WSLCVirtualMachine& virtualMachine, DockerEventTracker& eventTracker, const std::filesystem::path& storagePath);
    ~WSLCVolumes() = default;

    WSLCVolumeInformation CreateVolume(
        _In_opt_ LPCSTR Name,
        _In_opt_ LPCSTR Driver,
        _In_ std::map<std::string, std::string>&& DriverOpts,
        _In_ std::map<std::string, std::string>&& Labels);

    void DeleteVolume(_In_ LPCSTR Name);

    std::vector<WSLCVolumeInformation> ListVolumes() const;
    std::string InspectVolume(_In_ const std::string& Name) const;

private:
    void OpenVolume(_In_ const std::string& VolumeName);
    __requires_lock_held(m_lock) void OpenVolumeExclusiveLockHeld(const wsl::windows::common::docker_schema::Volume& vol);

    void OnVolumeEvent(const std::string& volumeName, VolumeEvent event, std::uint64_t eventTime);
    void OnVolumeDeleted(_In_ const std::string& VolumeName);

    mutable wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>> m_volumes;

    DockerHTTPClient& m_dockerClient;
    WSLCVirtualMachine& m_virtualMachine;
    std::filesystem::path m_storagePath;
    DockerEventTracker::EventTrackingReference m_volumeEventTracking;
};

} // namespace wsl::windows::service::wslc
