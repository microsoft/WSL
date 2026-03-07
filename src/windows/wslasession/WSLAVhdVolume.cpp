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

namespace {

    ULONGLONG ParseSizeBytes(const WSLAVolumeOptions& options)
    {
        if (options.Options == nullptr || options.Options[0] == '\0')
        {
            return c_defaultVolumeSizeBytes;
        }

        std::unordered_map<std::string, std::string> parsedOptions;
        try
        {
            parsedOptions = wsl::shared::FromJson<std::unordered_map<std::string, std::string>>(options.Options);
        }
        catch (...)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageWslaInvalidVolumeOptions(options.Options));
        }

        const auto it = parsedOptions.find("SizeBytes");
        if (it == parsedOptions.end())
        {
            return c_defaultVolumeSizeBytes;
        }

        const auto& sizeBytesString = it->second;
        if (sizeBytesString.empty())
        {
            return c_defaultVolumeSizeBytes;
        }

        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, wsl::shared::Localization::MessageInvalidSize(sizeBytesString.c_str()), sizeBytesString[0] == '-');

        ULONGLONG sizeBytes = 0;
        size_t parsedCount = 0;

        try
        {
            sizeBytes = std::stoull(sizeBytesString, &parsedCount);
        }
        catch (...)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageInvalidSize(sizeBytesString.c_str()));
        }

        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, wsl::shared::Localization::MessageInvalidSize(sizeBytesString.c_str()), parsedCount != sizeBytesString.size());

        if (sizeBytes == 0)
        {
            return c_defaultVolumeSizeBytes;
        }

        return sizeBytes;
    }

} // namespace

WSLAVhdVolumeImpl::WSLAVhdVolumeImpl(
    std::string&& Name, std::string&& Type, std::filesystem::path&& HostPath, ULONGLONG SizeBytes, ULONG Lun, std::string&& VirtualMachinePath, WSLAVirtualMachine* VirtualMachine) :
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
        Detach();
    }
    CATCH_LOG();
}

std::unique_ptr<WSLAVhdVolumeImpl> WSLAVhdVolumeImpl::Create(const WSLAVolumeOptions& Options, const std::filesystem::path& StoragePath, WSLAVirtualMachine& VirtualMachine)
{
    THROW_HR_IF_NULL(E_POINTER, Options.Name);
    THROW_HR_IF_NULL(E_POINTER, Options.Type);

    std::string name = Options.Name;
    std::string type = Options.Type;

    ULONGLONG sizeBytes = ParseSizeBytes(Options);

    const auto hostPath = StoragePath / "volumes" / (name + ".vhdx");

    auto createVhdCleanup =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(hostPath.c_str())); });

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
        std::move(name), std::move(type), std::filesystem::path(hostPath), sizeBytes, lun, std::move(virtualMachinePath), &VirtualMachine);

    mountCleanup.release();
    attachCleanup.release();
    createVhdCleanup.release();

    return volume;
}

void WSLAVhdVolumeImpl::Delete()
{
    Detach();

    auto hostPath = std::exchange(m_hostPath, std::filesystem::path{});
    if (!hostPath.empty())
    {
        LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(hostPath.c_str()));
    }
}

void WSLAVhdVolumeImpl::Detach()
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
}

} // namespace wsl::windows::service::wsla
