// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWSLCVolume.h"
#include "WSLCVolumeMetadata.h"
#include "DockerHTTPClient.h"
#include "DockerEventTracker.h"
#include <unordered_map>
#include <unordered_set>

namespace wsl::windows::service::wslc {

class WSLCVirtualMachine;

class WSLCVolumes
{
public:
    NON_COPYABLE(WSLCVolumes);
    NON_MOVABLE(WSLCVolumes);

    WSLCVolumes(DockerHTTPClient& dockerClient, WSLCVirtualMachine& virtualMachine, DockerEventTracker& eventTracker);
    ~WSLCVolumes() = default;

    WSLCVolumeInformation CreateVolume(
        _In_opt_ LPCSTR Name,
        _In_ const std::string& Driver,
        _In_ std::map<std::string, std::string>&& DriverOpts,
        _In_ std::map<std::string, std::string>&& Labels,
        _In_ const std::filesystem::path& StoragePath);

    void DeleteVolume(_In_ const std::string& Name);

    std::vector<WSLCVolumeInformation> ListVolumes() const;
    std::string InspectVolume(_In_ const std::string& Name) const;
    bool ContainsVolume(_In_ const std::string& Name) const;

    // Opens a volume by name. Used to track volumes Docker implicitly created for VOLUME directives.
    void OpenVolume(_In_ const std::string& VolumeName);

    // Removes a volume from tracking. Called when Docker deletes a volume (e.g. container delete with -v).
    void OnVolumeDeleted(_In_ const std::string& VolumeName);

private:
    std::unique_ptr<IWSLCVolume> OpenDockerVolume(const wsl::windows::common::docker_schema::Volume& vol);

    void OnVolumeEvent(const std::string& volumeName, VolumeEvent event, std::uint64_t eventTime);

    mutable wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>> m_volumes;

    DockerHTTPClient& m_dockerClient;
    WSLCVirtualMachine& m_virtualMachine;
    DockerEventTracker::EventTrackingReference m_volumeEventTracking;
};

} // namespace wsl::windows::service::wslc
