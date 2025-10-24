// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "DeviceHostProxy.h"

// This template works around a limitation with decltype on overloaded functions. It will be able
// to get the correct version of GetVmWorkerProcess based on the provided type arguments. By
// doing it this way, a compiler error will be generated if someone changes the signature of
// GetVmWorkerProcess.
//
// The way this works: decltype(GetVmWorkerProcess) does not work because it's overloaded.
// decltype(GetVmWorkerProcess(arg1, ...)) works to select an overload if you have values of the
// correct type (std::declval<T>() generates a value of the specified type), however the result
// of that is the function's return type, not the function's type, so the argument types must
// be repeated to reconstruct the function type.
template <typename... Args>
using GetVmWorkerProcessType = decltype(GetVmWorkerProcess(std::declval<Args>()...))(Args...);

// Limit the number of allowed doorbells registered by an external HDV vdev. Currently virtio-9p only uses
// one doorbell and wsldevicehost uses only two.
#define DEVICE_HOST_PROXY_DOORBELL_LIMIT 8

using namespace wsl::windows::common::hcs;

DeviceHostProxy::DeviceHostProxy(const std::wstring& VmId, const GUID& RuntimeId) :
    m_systemId{VmId}, m_runtimeId{RuntimeId}, m_system{wsl::windows::common::hcs::OpenComputeSystem(VmId.c_str(), GENERIC_ALL)}, m_shutdown{false}
{
    m_devicesShutdown = false;
}

GUID DeviceHostProxy::AddNewDevice(const GUID& Type, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs, const std::wstring& VirtIoTag)
{
    const wrl::ComPtr<IUnknown> thisUnknown{CastToUnknown()};
    GUID instanceId{};
    THROW_IF_FAILED(UuidCreate(&instanceId));
    // Tell the device host to create the device.
    THROW_IF_FAILED(Plan9Fs->CreateVirtioDevice(m_systemId.c_str(), thisUnknown.Get(), VirtIoTag.c_str(), &instanceId));

    // Add the instance ID to the list of known devices. This must be done before the device is
    // added to the system, because doing that can cause the register doorbell function to be
    // called.
    // N.B. It will be removed if there is a failure.
    {
        auto lock = m_devicesLock.lock_exclusive();
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

        m_devices.emplace(instanceId, DeviceHostProxyEntry{});
    }

    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(instanceId);
    });

    // Add the device to the compute system on behalf of the device host.
    ModifySettingRequest<FlexibleIoDevice> request;
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = L"VirtualMachine/Devices/FlexibleIov/";
    request.ResourcePath += wsl::shared::string::GuidToString<wchar_t>(instanceId, wsl::shared::string::GuidToStringFlags::None);
    request.Settings.EmulatorId = Type;
    request.Settings.HostingModel = FlexibleIoDeviceHostingModel::ExternalRestricted;
    wsl::windows::common::hcs::ModifyComputeSystem(m_system.get(), wsl::shared::ToJsonW(request).c_str());
    removeOnFailure.release();
    return instanceId;
}

void DeviceHostProxy::AddRemoteFileSystem(const GUID& ImplementationClsid, const std::wstring& Tag, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(E_CHANGED_STATE, m_shutdown);

    // Make sure there are no duplicate tags.
    for (auto& entry : m_fileSystems)
    {
        THROW_HR_IF(E_INVALIDARG, entry.ImplementationClsid == ImplementationClsid && entry.Tag == Tag);
    }

    m_fileSystems.emplace_back(ImplementationClsid, Tag, Plan9Fs);
}

wil::com_ptr<IPlan9FileSystem> DeviceHostProxy::GetRemoteFileSystem(const GUID& ImplementationClsid, std::wstring_view Tag)
{
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(E_CHANGED_STATE, m_shutdown);

    for (auto& entry : m_fileSystems)
    {
        if (entry.ImplementationClsid == ImplementationClsid && entry.Tag == Tag)
        {
            return entry.Instance;
        }
    }

    return {};
}

void DeviceHostProxy::Shutdown()
{
    {
        auto lock = m_lock.lock_exclusive();
        m_fileSystems.clear();
        m_shutdown = true;
    }

    {
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.clear();
        m_devicesShutdown = true;
    }
}

HRESULT
DeviceHostProxy::RegisterDeviceHost(_In_ IVmDeviceHost* DeviceHost, _In_ DWORD ProcessId, _Out_ UINT64* IpcSectionHandle)
try
{
    //
    // Because HdvProxyDeviceHost is not part of the API set, it is loaded here dynamically.
    //

    static LxssDynamicFunction<decltype(HdvProxyDeviceHost)> proxyDeviceHost{c_hdvModuleName, "HdvProxyDeviceHost"};
    const wil::com_ptr<IVmDeviceHost> remoteHost = DeviceHost;
    const wil::com_ptr<IUnknown> unknown = remoteHost.query<IUnknown>();
    THROW_IF_FAILED(proxyDeviceHost(m_system.get(), unknown.get(), ProcessId, IpcSectionHandle));
    return S_OK;
}
CATCH_RETURN()

HRESULT
DeviceHostProxy::NotifyAllDevicesInUse(_In_ LPCWSTR Tag)
try
{
    //
    // Add another Plan9 virtio device to the guest so additional mount commands will be possible.
    // This callback should be unused by virtiofs devices because a device is created for every
    // AddSharePath call.
    //
    auto p9fs = GetRemoteFileSystem(__uuidof(p9fs::Plan9FileSystem), Tag);
    THROW_HR_IF(E_NOT_SET, !p9fs);
    (void)AddNewDevice(VIRTIO_PLAN9_DEVICE_ID, p9fs, Tag);
    return S_OK;
}
CATCH_RETURN()

HRESULT
DeviceHostProxy::RegisterDoorbell(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event)
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    // Check if the device is one of the known devices that doorbells can be registered for, and
    // if the device has not already registered a doorbell.
    // N.B. For security it is enforced that each device can only register a small number of doorbells.
    //      Currently virtio-9p only uses one and the external virtio device uses two.
    const auto knownDevice = m_devices.find(InstanceId);
    RETURN_HR_IF(E_ACCESSDENIED, knownDevice == m_devices.end() || knownDevice->second.DoorbellCount == DEVICE_HOST_PROXY_DOORBELL_LIMIT);

    if (!knownDevice->second.MemoryNotification)
    {
        // Get an interface to the worker process to query devices.
        if (!m_deviceAccess)
        {
            static LxssDynamicFunction<GetVmWorkerProcessType<REFGUID, REFIID, IUnknown**>> getVmWorker{
                c_vmwpctrlModuleName, "GetVmWorkerProcess"};

            RETURN_IF_FAILED(getVmWorker(m_runtimeId, __uuidof(*m_deviceAccess), reinterpret_cast<IUnknown**>(&m_deviceAccess)));
        }

        RETURN_HR_IF(E_NOINTERFACE, !m_deviceAccess);

        // Retrieve the device's memory notification interface to register the doorbell, and store it
        // to be used during unregistration.
        wil::com_ptr<IUnknown> device;
        RETURN_IF_FAILED(m_deviceAccess->GetDevice(FLEXIO_DEVICE_ID, InstanceId, &device));
        knownDevice->second.MemoryNotification = device.query<IVmFiovGuestMemoryFastNotification>();
    }

    const auto result = knownDevice->second.MemoryNotification->RegisterDoorbell(
        static_cast<FIOV_BAR_SELECTOR>(BarIndex), Offset, TriggerValue, Flags, Event);

    if (SUCCEEDED(result))
    {
        ++knownDevice->second.DoorbellCount;
    }

    return result;
}
CATCH_RETURN()

HRESULT
DeviceHostProxy::UnregisterDoorbell(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags)
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    // Check if the device is a known device and has registered a doorbell.
    // N.B. If the device is being removed, the device can't be retrieved from the worker process
    //      so it's necessary to use the stored COM pointer.
    const auto device = m_devices.find(InstanceId);
    RETURN_HR_IF(E_ACCESSDENIED, device == m_devices.end() || device->second.DoorbellCount == 0);
    RETURN_IF_FAILED(device->second.MemoryNotification->UnregisterDoorbell(static_cast<FIOV_BAR_SELECTOR>(BarIndex), Offset, TriggerValue, Flags));

    if (--device->second.DoorbellCount == 0)
    {
        device->second.MemoryNotification.reset();
    }

    return S_OK;
}
CATCH_RETURN()

HRESULT
DeviceHostProxy::CreateSectionBackedMmioRange(
    const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages)
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    // Check if the device is one of the known devices.
    const auto knownDevice = m_devices.find(InstanceId);
    THROW_HR_IF(E_ACCESSDENIED, knownDevice == m_devices.end());

    if (!knownDevice->second.MemoryMapping)
    {
        // Get an interface to the worker process to query devices.
        if (!m_deviceAccess)
        {
            static LxssDynamicFunction<GetVmWorkerProcessType<REFGUID, REFIID, IUnknown**>> getVmWorker{
                c_vmwpctrlModuleName, "GetVmWorkerProcess"};
            THROW_IF_FAILED(getVmWorker(m_runtimeId, __uuidof(*m_deviceAccess), reinterpret_cast<IUnknown**>(&m_deviceAccess)));
        }

        THROW_HR_IF(E_NOINTERFACE, !m_deviceAccess);

        // Retrieve the device specific interface to manage mapped sections.
        wil::com_ptr<IUnknown> device;
        THROW_IF_FAILED(m_deviceAccess->GetDevice(FLEXIO_DEVICE_ID, InstanceId, &device));
        knownDevice->second.MemoryMapping = device.query<IVmFiovGuestMmioMappings>();
    }

    THROW_IF_FAILED(knownDevice->second.MemoryMapping->CreateSectionBackedMmioRange(
        static_cast<FIOV_BAR_SELECTOR>(BarIndex), BarOffsetInPages, PageCount, static_cast<FiovMmioMappingFlags>(MappingFlags), SectionHandle, SectionOffsetInPages));

    return S_OK;
}
CATCH_RETURN()

HRESULT
DeviceHostProxy::DestroySectionBackedMmioRange(const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages)
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);
    const auto device = m_devices.find(InstanceId);
    RETURN_HR_IF(E_ACCESSDENIED, device == m_devices.end() || !device->second.MemoryMapping);
    RETURN_IF_FAILED(device->second.MemoryMapping->DestroySectionBackedMmioRange(static_cast<FIOV_BAR_SELECTOR>(BarIndex), BarOffsetInPages));
    return S_OK;
}
CATCH_RETURN()