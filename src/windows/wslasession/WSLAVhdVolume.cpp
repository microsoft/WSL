/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVhdVolume.cpp

Abstract:

    Internal implementation for VHD-backed named volumes.

--*/

#include "precomp.h"
#include "WSLAVhdVolume.h"
#include "WSLAVirtualMachine.h"
#include "WslCoreFilesystem.h"

namespace wsl::windows::service::wsla {

static constexpr ULONGLONG c_defaultVolumeSizeBytes = 10ULL * 1024 * 1024 * 1024;

WSLAVhdVolumeImpl::WSLAVhdVolumeImpl(
    std::wstring&& Name,
    std::wstring&& Type,
    std::filesystem::path&& HostPath,
    ULONGLONG SizeBytes,
    ULONG Lun,
    std::string&& VirtualMachinePath,
    WSLAVirtualMachine* VirtualMachine) :
    m_name(std::move(Name)),
    m_type(std::move(Type)),
    m_hostPath(std::move(HostPath)),
    m_virtualMachinePath(std::move(VirtualMachinePath)),
    m_sizeBytes(SizeBytes),
    m_lun(Lun),
    m_virtualMachine(VirtualMachine)
{
}

WSLAVhdVolumeImpl::~WSLAVhdVolumeImpl()
{
    try
    {
        ReleaseResources();
    }
    CATCH_LOG();
}

std::unique_ptr<WSLAVhdVolumeImpl> WSLAVhdVolumeImpl::Create(
    const WSLA_VOLUME_OPTIONS& Options,
    const std::filesystem::path& StoragePath,
    WSLAVirtualMachine& VirtualMachine)
{
    THROW_HR_IF_NULL(E_POINTER, Options.Name);
    THROW_HR_IF_NULL(E_POINTER, Options.Type);

    std::wstring name = Options.Name;
    std::wstring type = Options.Type;

    std::map<std::wstring, std::wstring> options;

    if (Options.Options != nullptr)
    {
        options = wsl::shared::FromJson<std::map<std::wstring, std::wstring>>(Options.Options);
    }

    ULONGLONG sizeBytes = c_defaultVolumeSizeBytes;
    if (auto it = options.find(L"SizeBytes"); it != options.end())
    {
        sizeBytes = std::stoull(it->second);
    }

    THROW_HR_IF_MSG(E_INVALIDARG, sizeBytes == 0, "SizeBytes must be greater than 0");

    const auto hostPath = StoragePath / "volumes" / (name + L".vhdx");

    auto createVhdCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(hostPath.c_str()));
    });

    std::filesystem::create_directories(hostPath.parent_path());

    const auto tokenInfo = wil::get_token_information<TOKEN_USER>(GetCurrentProcessToken());
    wsl::core::filesystem::CreateVhd(hostPath.c_str(), sizeBytes, tokenInfo->User.Sid, false, false);

    auto [lun, device] = VirtualMachine.AttachDisk(hostPath.c_str(), false);
    auto attachCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.DetachDisk(lun); });

    VirtualMachine.Ext4Format(device);

    auto virtualMachinePath = std::format("/mnt/wsla-volumes/{}", name);
    VirtualMachine.Mount(device.c_str(), virtualMachinePath.c_str(), "ext4", "", 0);

    auto mountCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VirtualMachine.Unmount(virtualMachinePath.c_str()); });

    auto volume = std::make_unique<WSLAVhdVolumeImpl>(
        std::move(name),
        std::move(type),
        std::filesystem::path(hostPath),
        sizeBytes,
        lun,
        std::move(virtualMachinePath),
        &VirtualMachine);

    mountCleanup.release();
    attachCleanup.release();
    createVhdCleanup.release();

    return volume;
}

void WSLAVhdVolumeImpl::Delete()
{
    ReleaseResources();
}

void WSLAVhdVolumeImpl::ReleaseResources()
{
    if (m_virtualMachine != nullptr)
    {
        if (!m_virtualMachinePath.empty())
        {
            m_virtualMachine->Unmount(m_virtualMachinePath.c_str());
            m_virtualMachinePath.clear();
        }

        m_virtualMachine->DetachDisk(m_lun);
        m_virtualMachine = nullptr;
    }

    if (!m_hostPath.empty())
    {
        LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(m_hostPath.c_str()));
        m_hostPath.clear();
    }
}

} // namespace wsl::windows::service::wsla
