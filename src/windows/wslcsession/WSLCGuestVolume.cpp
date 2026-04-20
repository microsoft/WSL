/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCGuestVolume.cpp

Abstract:

    Implementation of WSLCGuestVolumeImpl - a WSLC volume whose storage is
    owned entirely by docker's built-in "local" driver inside the guest VM.

--*/

#include "precomp.h"
#include "DockerHTTPClient.h"
#include "WSLCGuestVolume.h"
#include "WSLCVolumeMetadata.h"
#include "wslc_schema.h"

using namespace wsl::windows::common;
using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

// Allowlist for driver options that the guest driver forwards to docker's
// "local" driver. Start conservatively: reject everything. Expand as concrete
// scenarios appear (e.g. tmpfs-backed volumes via type=tmpfs + filtered o=...).
// Rejecting unknown opts avoids accidentally exposing host paths via
// device=/host/path or other local-driver options that could break the
// WSL-for-apps isolation model.
void ValidateDriverOpts(const std::map<std::string, std::string>& DriverOpts)
{
    if (DriverOpts.empty())
    {
        return;
    }

    std::vector<std::string> keys(DriverOpts.size());
    std::ranges::transform(DriverOpts, keys.begin(), [](const auto& e) { return e.first; });

    auto optNames = wsl::shared::string::Join(keys, ',');

    THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcUnsupportedVolumeDriverOpts(optNames));
}

} // namespace

WSLCGuestVolumeImpl::WSLCGuestVolumeImpl(
    std::string&& Name,
    std::string&& CreatedAt,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    DockerHTTPClient& DockerClient) :
    m_name(std::move(Name)),
    m_createdAt(std::move(CreatedAt)),
    m_driverOpts(std::move(DriverOpts)),
    m_labels(std::move(Labels)),
    m_dockerClient(DockerClient)
{
}

std::unique_ptr<WSLCGuestVolumeImpl> WSLCGuestVolumeImpl::Create(
    LPCSTR Name,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    DockerHTTPClient& DockerClient)
{
    ValidateDriverOpts(DriverOpts);

    WSLCVolumeMetadata metadata;
    metadata.Driver = WSLCGuestVolumeDriver;
    metadata.DriverOpts = DriverOpts;

    docker_schema::CreateVolume request{};
    if (Name != nullptr && Name[0] != '\0')
    {
        request.Name = Name;
    }
    request.Driver = "local";
    request.DriverOpts = DriverOpts;
    request.Labels = {{WSLCVolumeMetadataLabel, wsl::shared::ToJson(metadata)}};

    // Merge user labels into the Docker volume labels.
    for (const auto& [key, value] : Labels)
    {
        request.Labels[key] = value;
    }

    try
    {
        auto createdVolume = DockerClient.CreateVolume(request);

        return std::make_unique<WSLCGuestVolumeImpl>(
            std::move(createdVolume.Name),
            std::move(createdVolume.CreatedAt),
            std::move(DriverOpts),
            std::move(Labels),
            DockerClient);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to create volume '%hs'", Name != nullptr ? Name : "");
}

std::unique_ptr<WSLCGuestVolumeImpl> WSLCGuestVolumeImpl::Open(const wsl::windows::common::docker_schema::Volume& Volume, DockerHTTPClient& DockerClient)
{
    THROW_HR_IF(E_INVALIDARG, !Volume.Labels.has_value());

    auto metadataIt = Volume.Labels->find(WSLCVolumeMetadataLabel);
    THROW_HR_IF(E_INVALIDARG, metadataIt == Volume.Labels->end());

    auto metadata = wsl::shared::FromJson<WSLCVolumeMetadata>(metadataIt->second.c_str());
    THROW_HR_IF(E_INVALIDARG, metadata.Driver != WSLCGuestVolumeDriver);

    // Extract user labels (all labels except our internal metadata label).
    std::map<std::string, std::string> userLabels;
    for (const auto& [key, value] : *Volume.Labels)
    {
        if (key != WSLCVolumeMetadataLabel)
        {
            userLabels[key] = value;
        }
    }

    auto volume = std::make_unique<WSLCGuestVolumeImpl>(
        std::string{Volume.Name},
        std::string{Volume.CreatedAt},
        std::move(metadata.DriverOpts),
        std::move(userLabels),
        DockerClient);

    return volume;
}

void WSLCGuestVolumeImpl::Delete()
{
    try
    {
        m_dockerClient.RemoveVolume(m_name);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), Localization::MessageWslcVolumeInUse(m_name.c_str()), e.StatusCode() == 409);
        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(m_name.c_str()), e.StatusCode() == 404);
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to delete volume '%hs'", m_name.c_str());
    }
}

std::string WSLCGuestVolumeImpl::Inspect() const
{
    wslc_schema::InspectVolume inspect{};
    inspect.Name = m_name;
    inspect.Driver = WSLCGuestVolumeDriver;
    inspect.CreatedAt = m_createdAt;
    inspect.DriverOpts = m_driverOpts;
    inspect.Labels = m_labels;

    return wsl::shared::ToJson(inspect);
}

WSLCVolumeInformation WSLCGuestVolumeImpl::GetVolumeInformation() const
{
    WSLCVolumeInformation Info{};

    THROW_HR_IF(E_UNEXPECTED, strcpy_s(Info.Name, m_name.c_str()) != 0);
    THROW_HR_IF(E_UNEXPECTED, strcpy_s(Info.Driver, WSLCGuestVolumeDriver) != 0);

    return Info;
}

} // namespace wsl::windows::service::wslc
