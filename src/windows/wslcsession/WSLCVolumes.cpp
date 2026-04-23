// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCVolumes.h"
#include "WSLCVhdVolume.h"
#include "WSLCGuestVolume.h"
#include "WSLCVirtualMachine.h"
#include "docker_schema.h"

using wsl::shared::Localization;
using wsl::windows::service::wslc::DockerHTTPClient;
using wsl::windows::service::wslc::WSLCVolumes;

namespace wsl::windows::service::wslc {

WSLCVolumes::WSLCVolumes(DockerHTTPClient& dockerClient, WSLCVirtualMachine& virtualMachine, DockerEventTracker& eventTracker) :
    m_dockerClient(dockerClient),
    m_virtualMachine(virtualMachine)
{
    // Recover existing volumes from Docker.
    for (const auto& volume : dockerClient.ListVolumes())
    {
        try
        {
            m_volumes.insert({volume.Name, OpenDockerVolume(volume)});
        }
        CATCH_LOG_MSG("Failed to recover volume: %hs", volume.Name.c_str());
    }

    // Register for volume events after recovery is complete.
    m_volumeEventTracking = eventTracker.RegisterVolumeUpdates(
        std::bind(&WSLCVolumes::OnVolumeEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

std::unique_ptr<IWSLCVolume> WSLCVolumes::OpenDockerVolume(const wsl::windows::common::docker_schema::Volume& vol)
{
    std::unique_ptr<IWSLCVolume> opened;

    if (vol.Labels.has_value() && vol.Labels->contains(WSLCVolumeMetadataLabel))
    {
        auto metadata = wsl::shared::FromJson<WSLCVolumeMetadata>(vol.Labels->at(WSLCVolumeMetadataLabel).c_str());

        if (metadata.Driver == WSLCVhdVolumeDriver)
        {
            opened = WSLCVhdVolumeImpl::Open(vol, m_virtualMachine, m_dockerClient);
        }
    }
    else if (vol.Driver == "local")
    {
        opened = WSLCGuestVolumeImpl::Open(vol, m_dockerClient);
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unrecognized volume driver: %hs", vol.Driver.c_str());
    }

    WSL_LOG("VolumeOpened", TraceLoggingValue(vol.Name.c_str(), "VolumeName"), TraceLoggingValue(vol.Driver.c_str(), "Driver"));

    return opened;
}

void WSLCVolumes::OnVolumeEvent(const std::string& volumeName, VolumeEvent event, std::uint64_t)
{
    if (event == VolumeEvent::Create)
    {
        OpenVolume(volumeName);
    }
    else if (event == VolumeEvent::Destroy)
    {
        OnVolumeDeleted(volumeName);
    }
}

WSLCVolumeInformation WSLCVolumes::CreateVolume(
    LPCSTR Name,
    const std::string& Driver,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    const std::filesystem::path& StoragePath)
{
    auto lock = m_lock.lock_exclusive();

    if (Name != nullptr && Name[0] != '\0')
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), m_volumes.contains(Name));
    }

    std::unique_ptr<IWSLCVolume> volume;

    if (Driver == WSLCVhdVolumeDriver)
    {
        volume = WSLCVhdVolumeImpl::Create(Name, std::move(DriverOpts), std::move(Labels), StoragePath, m_virtualMachine, m_dockerClient);
    }
    else if (Driver == WSLCGuestVolumeDriver)
    {
        volume = WSLCGuestVolumeImpl::Create(Name, std::move(DriverOpts), std::move(Labels), m_dockerClient);
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcInvalidVolumeType(Driver));
    }

    const auto& name = volume->Name();
    auto info = volume->GetVolumeInformation();

    auto [it, inserted] = m_volumes.insert({name, std::move(volume)});
    WI_VERIFY(inserted);

    WSL_LOG("VolumeCreated", TraceLoggingValue(name.c_str(), "VolumeName"), TraceLoggingValue(Driver.c_str(), "Driver"));

    return info;
}

void WSLCVolumes::DeleteVolume(const std::string& Name)
{
    auto lock = m_lock.lock_exclusive();

    auto it = m_volumes.find(Name);
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(Name), it == m_volumes.end());

    it->second->Delete();
    m_volumes.erase(it);

    WSL_LOG("VolumeDeleted", TraceLoggingValue(Name.c_str(), "VolumeName"));
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

void WSLCVolumes::OpenVolume(const std::string& VolumeName)
{
    auto lock = m_lock.lock_exclusive();

    if (VolumeName.empty() || m_volumes.contains(VolumeName))
    {
        return;
    }

    try
    {
        auto vol = m_dockerClient.InspectVolume(VolumeName);
        m_volumes.insert({VolumeName, OpenDockerVolume(vol)});
    }
    CATCH_LOG_MSG("Failed to open volume: %hs", VolumeName.c_str());
}

void WSLCVolumes::OnVolumeDeleted(const std::string& VolumeName)
{
    auto lock = m_lock.lock_exclusive();

    auto it = m_volumes.find(VolumeName);
    if (it != m_volumes.end())
    {
        WSL_LOG("VolumeRemovedWithContainer", TraceLoggingValue(VolumeName.c_str(), "VolumeName"));
        m_volumes.erase(it);
    }
}

} // namespace wsl::windows::service::wslc
