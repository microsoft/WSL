/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVolumes.cpp

Abstract:

    Contains the implementation of WSLCVolumes.

--*/

#include "precomp.h"
#include "WSLCVolumes.h"
#include "WSLCVhdVolume.h"
#include "WSLCGuestVolume.h"
#include "WSLCVirtualMachine.h"
#include "docker_schema.h"

using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

WSLCVolumes::WSLCVolumes(
    DockerHTTPClient& dockerClient, WSLCVirtualMachine& virtualMachine, DockerEventTracker& eventTracker, const std::filesystem::path& storagePath) :
    m_dockerClient(dockerClient), m_virtualMachine(virtualMachine), m_storagePath(storagePath)
{
    // Hold m_lock exclusively across both callback registration and the recovery loop.
    // This ensures any volume events that arrive while recovering are queued behind us in OnVolumeEvent,
    // and dedup naturally against entries inserted by recovery (insert is a no-op for existing keys).
    auto lock = m_lock.lock_exclusive();

    m_volumeEventTracking = eventTracker.RegisterVolumeUpdates(
        std::bind(&WSLCVolumes::OnVolumeEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    for (const auto& volume : dockerClient.ListVolumes())
    {
        try
        {
            OpenVolumeExclusiveLockHeld(volume);
        }
        CATCH_LOG_MSG("Failed to recover volume: %hs", volume.Name.c_str());
    }
}

__requires_lock_held(m_lock) void WSLCVolumes::OpenVolumeExclusiveLockHeld(const wsl::windows::common::docker_schema::Volume& vol)
{
    THROW_HR_IF_MSG(E_UNEXPECTED, vol.Driver != "local", "Unrecognized volume driver: %hs", vol.Driver.c_str());

    if (vol.Labels.has_value() && vol.Labels->contains(WSLCVolumeMetadataLabel))
    {
        auto metadata = wsl::shared::FromJson<WSLCVolumeMetadata>(vol.Labels->at(WSLCVolumeMetadataLabel).c_str());

        if (metadata.Driver == WSLCVhdVolumeDriver)
        {
            m_volumes.insert({vol.Name, WSLCVhdVolumeImpl::Open(vol, m_virtualMachine, m_dockerClient)});
            return;
        }
    }

    m_volumes.insert({vol.Name, WSLCGuestVolumeImpl::Open(vol, m_dockerClient)});
}

void WSLCVolumes::OnVolumeEvent(const std::string& volumeName, VolumeEvent event, std::uint64_t)
{
    auto lock = m_lock.lock_exclusive();

    // If this event matches the next self-initiated operation we are waiting to observe, the
    // map mutation has already been applied by CreateVolume / DeleteVolume. Just pop the event and
    // skip updating m_volumes.
    if (!m_expectedEvents.empty() && m_expectedEvents.front().first == volumeName && m_expectedEvents.front().second == event)
    {
        m_expectedEvents.pop_front();
        return;
    }

    if (event == VolumeEvent::Create)
    {
        OpenVolumeExclusiveLockHeld(volumeName);
    }
    else if (event == VolumeEvent::Destroy)
    {
        OnVolumeDeletedExclusiveLockHeld(volumeName);
    }
}

WSLCVolumeInformation WSLCVolumes::CreateVolume(
    LPCSTR Name, LPCSTR Driver, std::map<std::string, std::string>&& DriverOpts, std::map<std::string, std::string>&& Labels)
{
    auto lock = m_lock.lock_exclusive();

    if (Name != nullptr && Name[0] != '\0')
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_volumes.contains(Name));
    }

    std::string driver = (Driver != nullptr && Driver[0] != '\0') ? Driver : WSLCGuestVolumeDriver;
    std::unique_ptr<IWSLCVolume> volume;

    if (driver == WSLCVhdVolumeDriver)
    {
        volume = WSLCVhdVolumeImpl::Create(Name, std::move(DriverOpts), std::move(Labels), m_storagePath, m_virtualMachine, m_dockerClient);
    }
    else if (driver == WSLCGuestVolumeDriver)
    {
        volume = WSLCGuestVolumeImpl::Create(Name, std::move(DriverOpts), std::move(Labels), m_dockerClient);
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcInvalidVolumeType(driver));
    }

    const auto& name = volume->Name();
    auto info = volume->GetVolumeInformation();

    auto [it, inserted] = m_volumes.insert({name, std::move(volume)});
    WI_VERIFY(inserted);

    // Record that we initiated this create so OnVolumeEvent ignores the matching docker event.
    m_expectedEvents.emplace_back(name, VolumeEvent::Create);

    return info;
}

void WSLCVolumes::DeleteVolume(LPCSTR Name)
{
    THROW_HR_IF(E_POINTER, Name == nullptr);

    auto lock = m_lock.lock_exclusive();

    auto it = m_volumes.find(Name);
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(Name), it == m_volumes.end());

    it->second->Delete();
    m_volumes.erase(it);

    // Record that we initiated this destroy so OnVolumeEvent ignores the matching docker event.
    m_expectedEvents.emplace_back(Name, VolumeEvent::Destroy);
}

std::vector<WSLCVolumeInformation> WSLCVolumes::ListVolumes() const
{
    auto lock = m_lock.lock_shared();

    std::vector<WSLCVolumeInformation> result;
    result.reserve(m_volumes.size());

    for (const auto& [name, vol] : m_volumes)
    {
        result.push_back(vol->GetVolumeInformation());
    }

    return result;
}

std::string WSLCVolumes::InspectVolume(const std::string& Name) const
{
    auto lock = m_lock.lock_shared();

    auto it = m_volumes.find(Name);
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(Name), it == m_volumes.end());

    return it->second->Inspect();
}

bool WSLCVolumes::ContainsVolume(const std::string& Name) const
{
    auto lock = m_lock.lock_shared();
    return m_volumes.contains(Name);
}

__requires_lock_held(m_lock) void WSLCVolumes::OpenVolumeExclusiveLockHeld(const std::string& volumeName)
{
    if (volumeName.empty() || m_volumes.contains(volumeName))
    {
        return;
    }

    try
    {
        OpenVolumeExclusiveLockHeld(m_dockerClient.InspectVolume(volumeName));
    }
    CATCH_LOG_MSG("Failed to open volume: %hs", volumeName.c_str());
}

__requires_lock_held(m_lock) void WSLCVolumes::OnVolumeDeletedExclusiveLockHeld(const std::string& volumeName)
{
    auto it = m_volumes.find(volumeName);
    if (it != m_volumes.end())
    {
        it->second->OnDeleted();
        m_volumes.erase(it);
    }
}

} // namespace wsl::windows::service::wslc
