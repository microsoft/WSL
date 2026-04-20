/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVhdVolume.cpp

Abstract:

    Internal implementation for VHD-backed named volumes.

--*/

#include "precomp.h"
#include "DockerHTTPClient.h"
#include "WSLCVhdVolume.h"
#include "WSLCVirtualMachine.h"
#include "WSLCVolumeMetadata.h"
#include "WslCoreFilesystem.h"
#include "wslc_schema.h"

using namespace wsl::windows::common;
using wsl::shared::Localization;

namespace wsl::windows::service::wslc {

namespace {

    constexpr auto c_anonymousVolumeLabel = "com.docker.volume.anonymous";

    std::string GenerateName()
    {
        std::random_device rd;
        std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned short> random(rd());

        std::array<unsigned short, 32> randomBytes;
        std::generate(randomBytes.begin(), randomBytes.end(), random);

        std::string name;
        name.reserve(randomBytes.size() * 2);
        for (auto b : randomBytes)
        {
            std::format_to(std::back_inserter(name), "{:02x}", static_cast<BYTE>(b));
        }

        return name;
    }

    ULONGLONG ParseSizeBytes(std::map<std::string, std::string>& DriverOpts)
    {
        const auto it = DriverOpts.find("SizeBytes");
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcMissingVolumeOption("SizeBytes"), it == DriverOpts.end());

        auto& value = it->second;
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageInvalidSize(value), value[0] == '-');

        errno = 0;
        char* end = nullptr;
        auto sizeBytes = wsl::shared::string::ToUInt64(value.c_str(), &end);
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageInvalidSize(value), errno != 0 || *end != '\0' || sizeBytes == 0);

        return sizeBytes;
    }

} // namespace

WSLCVhdVolumeImpl::WSLCVhdVolumeImpl(
    std::string&& Name,
    std::filesystem::path&& HostPath,
    ULONGLONG SizeBytes,
    ULONG Lun,
    std::string&& Uuid,
    std::string&& CreatedAt,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    WSLCVirtualMachine& VirtualMachine,
    DockerHTTPClient& DockerClient) :
    m_name(std::move(Name)),
    m_hostPath(std::move(HostPath)),
    m_uuid(std::move(Uuid)),
    m_createdAt(std::move(CreatedAt)),
    m_driverOpts(std::move(DriverOpts)),
    m_labels(std::move(Labels)),
    m_sizeBytes(SizeBytes),
    m_lun(Lun),
    m_virtualMachine(VirtualMachine),
    m_dockerClient(DockerClient)
{
}

WSLCVhdVolumeImpl::~WSLCVhdVolumeImpl()
{
    Detach();
}

std::unique_ptr<WSLCVhdVolumeImpl> WSLCVhdVolumeImpl::Create(
    LPCSTR Name,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    const std::filesystem::path& StoragePath,
    WSLCVirtualMachine& VirtualMachine,
    DockerHTTPClient& DockerClient)
{
    auto name = (Name == nullptr || Name[0] == '\0') ? GenerateName() : std::string{Name};
    auto sizeBytes = ParseSizeBytes(DriverOpts);

    GUID uuidGuid{};
    THROW_IF_FAILED(CoCreateGuid(&uuidGuid));
    auto uuid = wsl::shared::string::GuidToString<char>(uuidGuid, wsl::shared::string::GuidToStringFlags::None);

    auto hostPath = StoragePath / "volumes" / (uuid + ".vhdx");

    auto createVhdCleanup =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(hostPath.c_str())); });

    std::filesystem::create_directories(hostPath.parent_path());

    const auto tokenInfo = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
    wsl::core::filesystem::CreateVhd(hostPath.c_str(), sizeBytes, tokenInfo->User.Sid, false, false);

    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    VirtualMachine.Ext4Format(device, uuid);

    WSLCVolumeMetadata metadata;
    metadata.Driver = WSLCVhdVolumeDriver;
    metadata.DriverOpts = DriverOpts;
    metadata.Properties = {
        {"HostPath", hostPath.string()},
        {"Uuid", uuid},
    };

    docker_schema::CreateVolume request{};
    request.Name = name;
    request.Driver = "local";
    request.DriverOpts = {
        {"type", "ext4"},
        {"device", "UUID=" + uuid},
    };
    request.Labels = {{WSLCVolumeMetadataLabel, wsl::shared::ToJson(metadata)}};

    // Merge user labels into the Docker volume labels.
    for (const auto& [key, value] : Labels)
    {
        request.Labels[key] = value;
    }

    try
    {
        auto createdVolume = DockerClient.CreateVolume(request);

        auto volume = std::make_unique<WSLCVhdVolumeImpl>(
            std::move(createdVolume.Name),
            std::move(hostPath),
            sizeBytes,
            lun,
            std::move(uuid),
            std::move(createdVolume.CreatedAt),
            std::move(DriverOpts),
            std::move(labels),
            VirtualMachine,
            DockerClient);

        attachCleanup.release();
        createVhdCleanup.release();

        return volume;
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to create volume '%hs'", Name != nullptr ? Name : "");
}

std::unique_ptr<WSLCVhdVolumeImpl> WSLCVhdVolumeImpl::Open(
    const wsl::windows::common::docker_schema::Volume& Volume, WSLCVirtualMachine& VirtualMachine, DockerHTTPClient& DockerClient)
{
    THROW_HR_IF(E_INVALIDARG, !Volume.Labels.has_value());

    auto metadataIt = Volume.Labels->find(WSLCVolumeMetadataLabel);
    THROW_HR_IF(E_INVALIDARG, metadataIt == Volume.Labels->end());

    auto metadata = wsl::shared::FromJson<WSLCVolumeMetadata>(metadataIt->second.c_str());
    THROW_HR_IF(E_INVALIDARG, metadata.Driver != WSLCVhdVolumeDriver);

    auto hostPathIt = metadata.Properties.find("HostPath");
    THROW_HR_IF(E_INVALIDARG, hostPathIt == metadata.Properties.end());
    THROW_HR_IF(E_INVALIDARG, hostPathIt->second.empty());

    auto hostPath = std::filesystem::path(hostPathIt->second);
    auto driverOpts = metadata.DriverOpts;
    auto sizeBytes = ParseSizeBytes(driverOpts);

    auto uuidIt = metadata.Properties.find("Uuid");
    THROW_HR_IF(E_INVALIDARG, uuidIt == metadata.Properties.end());
    THROW_HR_IF(E_INVALIDARG, uuidIt->second.empty());
    std::string uuid = uuidIt->second;

    // Extract user labels (all labels except our internal metadata label).
    std::map<std::string, std::string> userLabels;
    for (const auto& [key, value] : *Volume.Labels)
    {
        if (key != WSLCVolumeMetadataLabel)
        {
            userLabels[key] = value;
        }
    }

    // Attach the VHD so the kernel sees the ext4 superblock and libblkid can
    // resolve UUID=<uuid> when Docker mounts the device on container start.
    // No in-guest mount is needed.
    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    auto volume = std::make_unique<WSLCVhdVolumeImpl>(
        std::string{Volume.Name},
        std::move(hostPath),
        sizeBytes,
        lun,
        std::move(uuid),
        std::string{Volume.CreatedAt},
        std::move(driverOpts),
        std::move(userLabels),
        VirtualMachine,
        DockerClient);

    attachCleanup.release();

    return volume;
}

void WSLCVhdVolumeImpl::Delete()
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

    OnDeleted();
}

std::string WSLCVhdVolumeImpl::Inspect() const
{
    wslc_schema::InspectVolume inspect{};
    inspect.Name = m_name;
    inspect.Driver = WSLCVhdVolumeDriver;
    inspect.CreatedAt = m_createdAt;
    inspect.DriverOpts = m_driverOpts;
    inspect.Labels = m_labels;
    inspect.Status = std::map<std::string, std::string>{
        {"HostPath", m_hostPath.string()},
        {"SizeBytes", std::to_string(m_sizeBytes)},
    };

    return wsl::shared::ToJson(inspect);
}

WSLCVolumeInformation WSLCVhdVolumeImpl::GetVolumeInformation() const
{
    WSLCVolumeInformation Info{};

    THROW_HR_IF(E_UNEXPECTED, strcpy_s(Info.Name, m_name.c_str()) != 0);
    THROW_HR_IF(E_UNEXPECTED, strcpy_s(Info.Driver, WSLCVhdVolumeDriver) != 0);

    return Info;
}

void WSLCVhdVolumeImpl::OnDeleted()
{
    Detach();
    LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_hostPath.c_str()));
}

void WSLCVhdVolumeImpl::Detach()
try
{
    if (!m_attached)
    {
        return;
    }

    m_virtualMachine.DetachDisk(m_lun);
    m_attached = false;
}
CATCH_LOG();

} // namespace wsl::windows::service::wslc