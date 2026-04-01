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
    std::string SerializeVhdVolumeMetadata(const std::filesystem::path& HostPath, ULONGLONG SizeBytes)
    {
        WSLCVolumeMetadata metadata{};
        metadata.Type = WSLCVhdVolumeType;

        WSLCVhdVolumeMetadata vhdMetadata{};
        vhdMetadata.V1 = WSLCVhdVolumeMetadataV1{HostPath.wstring(), SizeBytes};
        metadata.VhdVolumeMetadata = std::move(vhdMetadata);

        return wsl::shared::ToJson(metadata);
    }

} // namespace

WSLCVhdVolumeImpl::WSLCVhdVolumeImpl(
    std::string&& Name,
    std::filesystem::path&& HostPath,
    ULONGLONG SizeBytes,
    ULONG Lun,
    std::string&& VirtualMachinePath,
    WSLCVirtualMachine& VirtualMachine,
    DockerHTTPClient& DockerClient) :
    m_name(std::move(Name)),
    m_hostPath(std::move(HostPath)),
    m_virtualMachinePath(std::move(VirtualMachinePath)),
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
    const WSLCVolumeOptions& Options, const std::filesystem::path& StoragePath, WSLCVirtualMachine& VirtualMachine, DockerHTTPClient& DockerClient)
{
    THROW_HR_IF_NULL(E_POINTER, Options.Name);
    THROW_HR_IF_NULL(E_POINTER, Options.Type);
    THROW_HR_IF_NULL(E_POINTER, Options.Options);

    std::string name = Options.Name;

    auto options = wsl::shared::FromJson<std::map<std::string, std::string>>(Options.Options);

    const auto it = options.find("SizeBytes");
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcInvalidVolumeOptions(Options.Options), it == options.end());

    auto sizeBytes = wsl::shared::FromJson<ULONGLONG>(it->second.c_str());
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageInvalidSize(it->second.c_str()), sizeBytes == 0);

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

    docker_schema::CreateVolume request{};
    request.Name = name;
    request.Driver = "local";
    request.DriverOpts = {
        {"type", "none"},
        {"o", "bind"},
        {"device", virtualMachinePath},
    };
    request.Labels = {{WSLCVolumeMetadataLabel, SerializeVhdVolumeMetadata(hostPath, sizeBytes)}};

    try
    {
        DockerClient.CreateVolume(request);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to create volume '%hs'", name.c_str());

    auto volume = std::make_unique<WSLCVhdVolumeImpl>(
        std::move(name), std::move(hostPath), sizeBytes, lun, std::move(virtualMachinePath), VirtualMachine, DockerClient);

    mountCleanup.release();
    attachCleanup.release();
    createVhdCleanup.release();

    return volume;
}

std::unique_ptr<WSLCVhdVolumeImpl> WSLCVhdVolumeImpl::Open(
    const wsl::windows::common::docker_schema::Volume& Volume, WSLCVirtualMachine& VirtualMachine, DockerHTTPClient& DockerClient)
{
    auto metadataIt = Volume.Labels.find(WSLCVolumeMetadataLabel);
    THROW_HR_IF(E_INVALIDARG, metadataIt == Volume.Labels.end());

    auto metadata = wsl::shared::FromJson<WSLCVolumeMetadata>(metadataIt->second.c_str());
    THROW_HR_IF(E_INVALIDARG, metadata.Type != WSLCVhdVolumeType);
    THROW_HR_IF(E_INVALIDARG, !metadata.VhdVolumeMetadata.has_value() || !metadata.VhdVolumeMetadata->V1.has_value());

    const auto& vhdMetadata = metadata.VhdVolumeMetadata->V1.value();
    THROW_HR_IF(E_INVALIDARG, vhdMetadata.HostPath.empty());
    THROW_HR_IF(E_INVALIDARG, vhdMetadata.SizeBytes == 0);

    auto deviceIt = Volume.Options.find("device");
    THROW_HR_IF(E_INVALIDARG, deviceIt == Volume.Options.end());
    THROW_HR_IF(E_INVALIDARG, deviceIt->second.empty());

    std::string virtualMachinePath = deviceIt->second;
    std::filesystem::path hostPath = vhdMetadata.HostPath;

    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    VirtualMachine.Mount(device.c_str(), virtualMachinePath.c_str(), "ext4", "", 0);
    auto mountCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.Unmount(virtualMachinePath.c_str()); });

    auto volume = std::make_unique<WSLCVhdVolumeImpl>(
        std::string{Volume.Name}, std::move(hostPath), vhdMetadata.SizeBytes, lun, std::move(virtualMachinePath), VirtualMachine, DockerClient);

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

    Detach();
    LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_hostPath.c_str()));
}

std::string WSLCVhdVolumeImpl::Inspect() const
{
    wslc_schema::InspectVolume inspect{};
    inspect.Name = m_name;
    inspect.Type = WSLCVhdVolumeType;
    inspect.VhdVolume = wslc_schema::InspectVhdVolume{
        m_hostPath.string(),
        m_sizeBytes,
    };

    return wsl::shared::ToJson(inspect);
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
