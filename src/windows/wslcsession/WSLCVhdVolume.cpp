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
    std::string&& VirtualMachinePath,
    std::map<std::string, std::string>&& DriverOpts,
    std::map<std::string, std::string>&& Labels,
    WSLCVirtualMachine& VirtualMachine,
    DockerHTTPClient& DockerClient) :
    m_name(std::move(Name)),
    m_hostPath(std::move(HostPath)),
    m_virtualMachinePath(std::move(VirtualMachinePath)),
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
    std::string name = (Name != nullptr && Name[0] != '\0') ? std::string(Name) : GenerateName();
    auto sizeBytes = ParseSizeBytes(DriverOpts);
    auto hostPath = StoragePath / "volumes" / (name + ".vhdx");

    auto createVhdCleanup =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(hostPath.c_str())); });

    std::filesystem::create_directories(hostPath.parent_path());

    const auto tokenInfo = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
    wsl::core::filesystem::CreateVhd(hostPath.c_str(), sizeBytes, tokenInfo->User.Sid, false, false);

    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    VirtualMachine.Ext4Format(device);

    auto virtualMachinePath = std::format("/mnt/wslc-volumes/{}", name);
    VirtualMachine.Mount(device.c_str(), virtualMachinePath.c_str(), "ext4", "", 0);

    auto mountCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.Unmount(virtualMachinePath.c_str()); });

    WSLCVolumeMetadata metadata;
    metadata.Driver = WSLCVhdVolumeDriver;
    metadata.DriverOpts = DriverOpts;
    metadata.Properties = {
        {"HostPath", hostPath.string()},
    };

    docker_schema::CreateVolume request{};
    request.Name = name;
    request.Driver = "local";
    request.DriverOpts = {
        {"type", "none"},
        {"o", "bind"},
        {"device", virtualMachinePath},
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
            std::move(name), std::move(hostPath), sizeBytes, lun, std::move(virtualMachinePath), std::move(DriverOpts), std::move(Labels), VirtualMachine, DockerClient);
        volume->m_createdAt = createdVolume.CreatedAt;

        mountCleanup.release();
        attachCleanup.release();
        createVhdCleanup.release();

        return volume;
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to create volume '%hs'", name.c_str());
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

    THROW_HR_IF(E_INVALIDARG, !Volume.Options.has_value());
    auto deviceIt = Volume.Options->find("device");
    THROW_HR_IF(E_INVALIDARG, deviceIt == Volume.Options->end());
    THROW_HR_IF(E_INVALIDARG, deviceIt->second.empty());
    std::string virtualMachinePath = deviceIt->second;

    // Extract user labels (all labels except our internal metadata label).
    std::map<std::string, std::string> userLabels;
    for (const auto& [key, value] : *Volume.Labels)
    {
        if (key != WSLCVolumeMetadataLabel)
        {
            userLabels[key] = value;
        }
    }

    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    VirtualMachine.Mount(device.c_str(), virtualMachinePath.c_str(), "ext4", "", 0);
    auto mountCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.Unmount(virtualMachinePath.c_str()); });

    auto volume = std::make_unique<WSLCVhdVolumeImpl>(
        std::string{Volume.Name}, std::move(hostPath), sizeBytes, lun, std::move(virtualMachinePath), std::move(driverOpts), std::move(userLabels), VirtualMachine, DockerClient);
    volume->m_createdAt = Volume.CreatedAt;

    mountCleanup.release();
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

    if (!m_virtualMachinePath.empty())
    {
        m_virtualMachine.Unmount(m_virtualMachinePath.c_str());
        m_virtualMachinePath.clear();
    }

    m_virtualMachine.DetachDisk(m_lun);
    m_attached = false;
}
CATCH_LOG();

} // namespace wsl::windows::service::wslc