// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windowsdefs.h>
#include "hcs.hpp"

namespace wrl = Microsoft::WRL;

class DeviceHostProxy : public wrl::RuntimeClass<wrl::RuntimeClassFlags<wrl::RuntimeClassType::ClassicCom>, IVmDeviceHostSupport, IPlan9FileSystemHost>
{
public:
    DeviceHostProxy(const std::wstring& VmId, const GUID& RuntimeId);

    GUID AddNewDevice(const GUID& Type, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs, const std::wstring& VirtIoTag);

    void RemoveDevice(const GUID& Type, const GUID& InstanceId);

    void AddRemoteFileSystem(const GUID& ImplementationClsid, const std::wstring& Tag, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs);

    wil::com_ptr<IPlan9FileSystem> GetRemoteFileSystem(const GUID& ImplementationClsid, std::wstring_view Tag);

    void Shutdown();

    //
    // IVmDeviceHostSupport
    //
    IFACEMETHOD(RegisterDeviceHost)(_In_ IVmDeviceHost* DeviceHost, _In_ DWORD ProcessId, _Out_ UINT64* IpcSectionHandle) override;

    //
    // IPlan9FileSystemHost
    //
    IFACEMETHOD(NotifyAllDevicesInUse)(_In_ LPCWSTR Tag) override;

    IFACEMETHOD(RegisterDoorbell)(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event) override;

    IFACEMETHOD(UnregisterDoorbell)(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags) override;

    IFACEMETHOD(CreateSectionBackedMmioRange)(
        const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages) override;

    IFACEMETHOD(DestroySectionBackedMmioRange)(const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages) override;

private:
    struct RemoteFileSystemInfo
    {
        RemoteFileSystemInfo(GUID ImplementationClsid, const std::wstring& Tag, const wil::com_ptr<IPlan9FileSystem>& Instance, IGlobalInterfaceTable* git) :
            ImplementationClsid{ImplementationClsid}, Tag{Tag}, m_git{git}
        {
            THROW_IF_FAILED(git->RegisterInterfaceInGlobal(Instance.get(), __uuidof(IPlan9FileSystem), &Cookie));
        }

        ~RemoteFileSystemInfo()
        {
            if (Cookie != 0)
            {
                LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(Cookie));
            }
        }

        RemoteFileSystemInfo(RemoteFileSystemInfo&& other) noexcept
        {
            *this = std::move(other);
        }

        RemoteFileSystemInfo& operator=(RemoteFileSystemInfo&& other) noexcept
        {
            if (this != &other)
            {
                if (Cookie != 0)
                {
                    LOG_IF_FAILED(m_git->RevokeInterfaceFromGlobal(Cookie));
                }

                ImplementationClsid = other.ImplementationClsid;
                Tag = std::move(other.Tag);
                Cookie = other.Cookie;
                m_git = other.m_git;
                other.Cookie = 0;
            }

            return *this;
        }

        RemoteFileSystemInfo(const RemoteFileSystemInfo&) = delete;
        RemoteFileSystemInfo& operator=(const RemoteFileSystemInfo&) = delete;

        GUID ImplementationClsid{};
        std::wstring Tag;
        DWORD Cookie = 0;

    private:
        IGlobalInterfaceTable* m_git = nullptr;
    };

    wil::com_ptr<IGlobalInterfaceTable> m_git;

    std::wstring m_systemId;
    GUID m_runtimeId;
    wsl::windows::common::hcs::unique_hcs_system m_system;
    wil::srwlock m_lock;
    std::vector<RemoteFileSystemInfo> m_fileSystems;
    bool m_shutdown;

    struct DeviceHostProxyEntry
    {
        wil::com_ptr<IVmFiovGuestMemoryFastNotification> MemoryNotification;
        wil::com_ptr<IVmFiovGuestMmioMappings> MemoryMapping;
        size_t DoorbellCount = 0;
    };

    wil::com_ptr<IVmVirtualDeviceAccess> m_deviceAccess;
    wil::srwlock m_devicesLock;
    std::map<GUID, DeviceHostProxyEntry, wsl::windows::common::helpers::GuidLess> m_devices;
    bool m_devicesShutdown;

    static constexpr LPCWSTR c_hdvModuleName = L"vmdevicehost.dll";
    static constexpr LPCWSTR c_vmwpctrlModuleName = L"vmwpctrl.dll";
};