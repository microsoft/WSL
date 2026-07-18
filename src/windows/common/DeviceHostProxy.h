// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windowsdefs.h>
#include <WslDeviceHost.h>
#include "hcs.hpp"

namespace wrl = Microsoft::WRL;

class DeviceHostProxy
    : public wrl::RuntimeClass<wrl::RuntimeClassFlags<wrl::RuntimeClassType::ClassicCom>, IVmDeviceHostSupport, IPlan9FileSystemHost, IWslDeviceHostCallback>
{
public:
    DeviceHostProxy(const std::wstring& VmId, const GUID& RuntimeId, bool EnableTelemetry = true);

    GUID AddNewDevice(const GUID& Type, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs, const std::wstring& VirtIoTag);

    GUID AddVirtioNetDevice(_In_ HANDLE UserToken, const WslVirtioNetConfig& Config, const std::vector<IpAddress>& Nameservers);

    GUID AddVirtiofsDevice(
        _In_ HANDLE UserToken, const std::wstring& Label, const std::wstring& RootPath, VirtiofsShareKind Kind, UINT32 ShmemSizeMb, const std::wstring& MountOptions);

    GUID AddVirtioPmemDevice(_In_ HANDLE UserToken, const std::wstring& Path, bool Writable);

    void RemoveDevice(const GUID& InstanceId);

    void AddRemoteFileSystem(const GUID& ImplementationClsid, const std::wstring& Tag, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs);

    wil::com_ptr<IPlan9FileSystem> GetRemoteFileSystem(const GUID& ImplementationClsid, std::wstring_view Tag);

    wil::com_ptr<IWslVirtioNetDevice> GetVirtioNetDevice(const GUID& InstanceId);

    void SetSwiotlb(UINT64 GpaBase, UINT64 SizeBytes);

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

    //
    // IWslDeviceHostCallback
    //
    IFACEMETHOD(RegisterDoorbell)(GUID InstanceId, BYTE BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event) override;

    IFACEMETHOD(UnregisterDoorbell)(GUID InstanceId, BYTE BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags) override;

    IFACEMETHOD(CreateSectionBackedMmioRange)(
        GUID InstanceId, BYTE BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages) override;

    IFACEMETHOD(DestroySectionBackedMmioRange)(GUID InstanceId, BYTE BarIndex, UINT64 BarOffsetInPages) override;

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

    struct DeviceHostProxyEntry;

    wil::com_ptr<IWslVm> GetWslVm(_In_ HANDLE UserToken);
    wil::com_ptr<IWslDeviceHostCallback> GetCallback();
    void AddFlexibleIoDevice(const GUID& Type, const GUID& InstanceId);
    _Requires_lock_held_(m_lock)
    void ConfigureSwiotlb(const wil::com_ptr<IWslVm>& Vm, bool& Configured);
    void TeardownDevice(const wil::com_ptr<IUnknown>& Device) noexcept;

    HRESULT RegisterDoorbellImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event) noexcept;
    HRESULT UnregisterDoorbellImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags) noexcept;
    HRESULT CreateSectionBackedMmioRangeImpl(
        const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages) noexcept;
    HRESULT DestroySectionBackedMmioRangeImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages) noexcept;

    std::wstring m_systemId;
    GUID m_runtimeId;
    bool m_enableTelemetry;
    wsl::windows::common::hcs::unique_hcs_system m_system;
    wil::srwlock m_lock;
    std::vector<RemoteFileSystemInfo> m_fileSystems;
    bool m_shutdown;
    wil::com_ptr<IWslVm> m_wslVm;
    wil::com_ptr<IWslVm> m_adminWslVm;
    SwiotlbConfig m_swiotlbConfig{};
    bool m_swiotlbConfigured = false;
    bool m_wslVmSwiotlbConfigured = false;
    bool m_adminWslVmSwiotlbConfigured = false;

    struct DeviceHostProxyEntry
    {
        GUID Type{};
        wil::com_ptr<IVmFiovGuestMemoryFastNotification> MemoryNotification;
        wil::com_ptr<IVmFiovGuestMmioMappings> MemoryMapping;
        wil::com_ptr<IUnknown> Device;
        size_t DoorbellCount = 0;
        bool ShuttingDown = false;
    };

    wil::com_ptr<IVmVirtualDeviceAccess> m_deviceAccess;
    std::mutex m_deviceLifecycleLock;
    wil::srwlock m_devicesLock;
    std::map<GUID, DeviceHostProxyEntry, wsl::windows::common::helpers::GuidLess> m_devices;
    bool m_devicesShutdown;

    // A kill-on-close job per device host process, held for the proxy's lifetime so the
    // processes are terminated when the VM shuts down. Guarded by m_devicesLock.
    std::vector<wil::unique_handle> m_processJobs;

    static constexpr LPCWSTR c_hdvModuleName = L"vmdevicehost.dll";
    static constexpr LPCWSTR c_vmwpctrlModuleName = L"vmwpctrl.dll";
};