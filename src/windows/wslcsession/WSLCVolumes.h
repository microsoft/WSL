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

    // Atomically create a guest-driver volume with default options if a volume
    // with this name does not already exist. Used by the container-create path
    // to synchronously own named volumes referenced by --volume, bypassing the
    // podman /events stream which emits volume.create asynchronously (or not
    // at all) for container-create-driven implicit creates. Returns true if a
    // new volume was created, false if it already existed.
    bool EnsureVolumeExists(_In_ const std::string& Name);

    // Adopt an already-existing backend volume into tracking if it is not
    // tracked yet. Used by the container-create path to register anonymous
    // volumes that podman creates implicitly for a container (e.g. from an
    // image VOLUME instruction). Unlike EnsureVolumeExists, this never creates
    // the volume and never enqueues an expected create event, since the volume
    // already exists and no further create event is expected. No-op if the
    // volume is already tracked or does not exist in the backend.
    void TrackExistingVolume(_In_ const std::string& Name);

    void DeleteVolume(_In_ LPCSTR Name);

    std::vector<WSLCVolumeInformation> ListVolumes(std::map<std::string, std::vector<std::string>>&& Filters) const;

    struct PruneVolumesResult
    {
        std::vector<std::string> Volumes;
        std::uint64_t SpaceReclaimed{};
    };

    PruneVolumesResult PruneVolumes(_In_ const std::map<std::string, std::vector<std::string>>& Filters);

    std::string InspectVolume(_In_ const std::string& Name) const;

    std::pair<HRESULT, std::string> GetVolumeStatus(_In_ const std::string& Name) const;

private:
    __requires_lock_held(m_lock) void OpenVolumeExclusiveLockHeld(const wsl::windows::common::docker_schema::Volume& vol);
    __requires_lock_held(m_lock) void OpenVolumeExclusiveLockHeld(const std::string& volumeName);
    __requires_lock_held(m_lock) void OnVolumeDeletedExclusiveLockHeld(const std::string& volumeName);

    void OnVolumeEvent(const std::string& volumeName, VolumeEvent event, std::uint64_t eventTime);

    mutable wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>> m_volumes;
    _Guarded_by_(m_lock) std::deque<std::pair<std::string, VolumeEvent>> m_expectedEvents;

    DockerHTTPClient& m_dockerClient;
    WSLCVirtualMachine& m_virtualMachine;
    std::filesystem::path m_storagePath;
    DockerEventTracker::EventTrackingReference m_volumeEventTracking;
};

} // namespace wsl::windows::service::wslc
