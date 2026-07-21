// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "DeviceHostProxy.h"
#include "WslSecurity.h"

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

namespace {
constexpr GUID c_virtioFsDeviceId{0x872270E1, 0xA899, 0x4AF6, {0xB4, 0x54, 0x71, 0x93, 0x63, 0x44, 0x35, 0xAD}};
constexpr GUID c_virtioNetDeviceId{0xF07010D0, 0x0EA9, 0x447F, {0x88, 0xEF, 0xBD, 0x95, 0x2A, 0x4D, 0x2F, 0x14}};
constexpr GUID c_virtioPmemDeviceId{0xEDBB24BB, 0x5E19, 0x40F4, {0x8A, 0x0F, 0x82, 0x24, 0x31, 0x30, 0x64, 0xFD}};
} // namespace

DeviceHostProxy::DeviceHostProxy(const std::wstring& VmId, const GUID& RuntimeId, bool EnableTelemetry) :
    m_systemId{VmId},
    m_runtimeId{RuntimeId},
    m_enableTelemetry{EnableTelemetry},
    m_system{wsl::windows::common::hcs::OpenComputeSystem(VmId.c_str(), GENERIC_ALL)},
    m_shutdown{false}
{
    m_devicesShutdown = false;
    m_git = wil::CoCreateInstance<IGlobalInterfaceTable>(CLSID_StdGlobalInterfaceTable, CLSCTX_INPROC_SERVER);
}

GUID DeviceHostProxy::AddNewDevice(const GUID& Type, const wil::com_ptr<IPlan9FileSystem>& Plan9Fs, const std::wstring& VirtIoTag)
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);

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

        m_devices.emplace(instanceId, DeviceHostProxyEntry{.Type = Type});
    }

    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(instanceId);
    });

    // Add the device to the compute system on behalf of the device host.
    AddFlexibleIoDevice(Type, instanceId);
    removeOnFailure.release();
    return instanceId;
}

GUID DeviceHostProxy::AddVirtioNetDevice(_In_ HANDLE UserToken, const WslVirtioNetConfig& Config, const std::vector<IpAddress>& Nameservers)
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);

    GUID instanceId{};
    THROW_IF_FAILED(UuidCreate(&instanceId));

    {
        auto lock = m_devicesLock.lock_exclusive();
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);
        m_devices.emplace(instanceId, DeviceHostProxyEntry{.Type = c_virtioNetDeviceId});
    }

    wil::com_ptr<IUnknown> device;
    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        TeardownDevice(device);
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(instanceId);
    });
    wil::com_ptr<IWslVirtioNetDevice> netDevice;
    {
        auto instanceIdForCall = instanceId;
        auto config = Config;
        auto nameservers = Nameservers;
        IpAddress emptyNameserver{};
        auto* nameserversData = nameservers.empty() ? &emptyNameserver : nameservers.data();
        THROW_IF_FAILED(GetWslVm(UserToken)->CreateVirtioNetDevice(
            &instanceIdForCall,
            GetCallback().get(),
            &config,
            gsl::narrow_cast<UINT32>(nameservers.size()),
            nameserversData,
            netDevice.put()));
    }
    device = netDevice.query<IUnknown>();

    {
        auto lock = m_devicesLock.lock_exclusive();
        const auto entry = m_devices.find(instanceId);
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown || entry == m_devices.end());
        entry->second.Device = device;
    }

    AddFlexibleIoDevice(c_virtioNetDeviceId, instanceId);
    removeOnFailure.release();
    return instanceId;
}

GUID DeviceHostProxy::AddVirtiofsDevice(
    _In_ HANDLE UserToken, const std::wstring& Label, const std::wstring& RootPath, VirtiofsShareKind Kind, UINT32 ShmemSizeMb, const std::wstring& MountOptions)
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);

    GUID instanceId{};
    THROW_IF_FAILED(UuidCreate(&instanceId));

    {
        auto lock = m_devicesLock.lock_exclusive();
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);
        m_devices.emplace(instanceId, DeviceHostProxyEntry{.Type = c_virtioFsDeviceId});
    }

    wil::com_ptr<IUnknown> device;
    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        TeardownDevice(device);
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(instanceId);
    });
    wil::com_ptr<IWslVirtiofsDevice> virtiofsDevice;
    {
        auto instanceIdForCall = instanceId;
        const auto label = wil::make_bstr(Label.c_str());
        const auto rootPath = wil::make_bstr(RootPath.c_str());
        const auto mountOptions = wil::make_bstr(MountOptions.c_str());
        WslVirtiofsConfig config{
            .label = label.get(),
            .rootPath = rootPath.get(),
            .kind = Kind,
            .shmemSizeMb = ShmemSizeMb,
            // To workaround memory aperture limitations, limit virtiofs devices to one queue.
            .queueCount = 1,
            .mountOptions = mountOptions.get()};
        THROW_IF_FAILED(GetWslVm(UserToken)->CreateVirtiofsDevice(&instanceIdForCall, GetCallback().get(), &config, virtiofsDevice.put()));
    }
    device = virtiofsDevice.query<IUnknown>();

    {
        auto lock = m_devicesLock.lock_exclusive();
        const auto entry = m_devices.find(instanceId);
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown || entry == m_devices.end());
        entry->second.Device = device;
    }

    AddFlexibleIoDevice(c_virtioFsDeviceId, instanceId);
    removeOnFailure.release();
    return instanceId;
}

GUID DeviceHostProxy::AddVirtioPmemDevice(_In_ HANDLE UserToken, const std::wstring& Path, bool Writable)
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);

    GUID instanceId{};
    THROW_IF_FAILED(UuidCreate(&instanceId));

    {
        auto lock = m_devicesLock.lock_exclusive();
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);
        m_devices.emplace(instanceId, DeviceHostProxyEntry{.Type = c_virtioPmemDeviceId});
    }

    wil::com_ptr<IUnknown> device;
    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        TeardownDevice(device);
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(instanceId);
    });
    wil::com_ptr<IWslVirtioPmemDevice> pmemDevice;
    {
        auto instanceIdForCall = instanceId;
        const auto path = wil::make_bstr(Path.c_str());
        WslVirtioPmemConfig config{.path = path.get(), .writable = Writable};
        THROW_IF_FAILED(GetWslVm(UserToken)->CreateVirtioPmemDevice(&instanceIdForCall, GetCallback().get(), &config, pmemDevice.put()));
    }
    device = pmemDevice.query<IUnknown>();

    {
        auto lock = m_devicesLock.lock_exclusive();
        const auto entry = m_devices.find(instanceId);
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown || entry == m_devices.end());
        entry->second.Device = device;
    }

    AddFlexibleIoDevice(c_virtioPmemDeviceId, instanceId);
    removeOnFailure.release();
    return instanceId;
}

void DeviceHostProxy::AddFlexibleIoDevice(const GUID& Type, const GUID& InstanceId)
{
    ModifySettingRequest<FlexibleIoDevice> request;
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = L"VirtualMachine/Devices/FlexibleIov/";
    request.ResourcePath += wsl::shared::string::GuidToString<wchar_t>(InstanceId, wsl::shared::string::GuidToStringFlags::None);
    request.Settings.EmulatorId = Type;
    request.Settings.HostingModel = FlexibleIoDeviceHostingModel::ExternalRestricted;
    wsl::windows::common::hcs::ModifyComputeSystem(m_system.get(), wsl::shared::ToJsonW(request).c_str());
}

void DeviceHostProxy::RemoveDevice(const GUID& InstanceId)
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);
    wil::com_ptr<IUnknown> device;
    GUID type{};

    {
        auto lock = m_devicesLock.lock_exclusive();
        const auto entry = m_devices.find(InstanceId);
        THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);
        THROW_HR_IF(E_INVALIDARG, entry == m_devices.end());
        entry->second.ShuttingDown = true;
        device = entry->second.Device;
        type = entry->second.Type;
    }

    TeardownDevice(device);

    {
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.erase(InstanceId);
    }

    // N.B. Removing the FlexIov device is best effort since not all versions of Windows support it.
    try
    {
        ModifySettingRequest<FlexibleIoDevice> request;
        request.RequestType = ModifyRequestType::Remove;
        request.ResourcePath = L"VirtualMachine/Devices/FlexibleIov/";
        request.ResourcePath += wsl::shared::string::GuidToString<wchar_t>(InstanceId, wsl::shared::string::GuidToStringFlags::None);
        request.Settings.EmulatorId = type;
        request.Settings.HostingModel = FlexibleIoDeviceHostingModel::ExternalRestricted;
        wsl::windows::common::hcs::ModifyComputeSystem(m_system.get(), wsl::shared::ToJsonW(request).c_str());
    }
    CATCH_LOG()
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

    m_fileSystems.emplace_back(ImplementationClsid, Tag, Plan9Fs, m_git.get());
}

wil::com_ptr<IPlan9FileSystem> DeviceHostProxy::GetRemoteFileSystem(const GUID& ImplementationClsid, std::wstring_view Tag)
{
    auto lock = m_lock.lock_shared();
    THROW_HR_IF(E_CHANGED_STATE, m_shutdown);

    for (auto& entry : m_fileSystems)
    {
        if (entry.ImplementationClsid == ImplementationClsid && entry.Tag == Tag)
        {
            // Retrieve the instance from the global interface table to ensure the correct apartment/thread affinity.
            // This is required because we might be running under MTA or NA depending on which class we were called from.

            wil::com_ptr<IPlan9FileSystem> instance;
            THROW_IF_FAILED(
                m_git->GetInterfaceFromGlobal(entry.Cookie, __uuidof(IPlan9FileSystem), reinterpret_cast<void**>(instance.put())));
            return instance;
        }
    }

    return {};
}

wil::com_ptr<IWslVirtioNetDevice> DeviceHostProxy::GetVirtioNetDevice(const GUID& InstanceId)
{
    auto lock = m_devicesLock.lock_shared();
    THROW_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    const auto device = m_devices.find(InstanceId);
    THROW_HR_IF(E_NOT_SET, device == m_devices.end() || device->second.ShuttingDown || !device->second.Device);
    return device->second.Device.query<IWslVirtioNetDevice>();
}

void DeviceHostProxy::SetSwiotlb(UINT64 GpaBase, UINT64 SizeBytes)
{
    if (GpaBase == 0 && SizeBytes == 0)
    {
        return;
    }

    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(E_CHANGED_STATE, m_shutdown);

    SwiotlbConfig config{.gpaBase = GpaBase, .sizeBytes = SizeBytes};
    if (m_swiotlbConfigured)
    {
        THROW_HR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            m_swiotlbConfig.gpaBase != config.gpaBase || m_swiotlbConfig.sizeBytes != config.sizeBytes);
    }
    else
    {
        m_swiotlbConfig = config;
        m_swiotlbConfigured = true;
    }

    ConfigureSwiotlb(m_wslVm, m_wslVmSwiotlbConfigured);
    ConfigureSwiotlb(m_adminWslVm, m_adminWslVmSwiotlbConfigured);
}

void DeviceHostProxy::Shutdown()
{
    std::lock_guard lifecycleLock(m_deviceLifecycleLock);

    {
        auto lock = m_lock.lock_exclusive();
        m_fileSystems.clear();
        m_shutdown = true;
    }

    std::vector<wil::com_ptr<IUnknown>> devices;
    {
        auto lock = m_devicesLock.lock_exclusive();

        // Block device retrieval and new registrations while retaining the entries needed by
        // Teardown() callbacks to unregister doorbells and destroy mapped ranges.
        m_devicesShutdown = true;
        devices.reserve(m_devices.size());
        for (auto& device : m_devices)
        {
            device.second.ShuttingDown = true;
            devices.emplace_back(device.second.Device);
        }
    }

    for (const auto& device : devices)
    {
        TeardownDevice(device);
    }

    {
        auto lock = m_devicesLock.lock_exclusive();
        m_devices.clear();
    }
}

wil::com_ptr<IWslVm> DeviceHostProxy::GetWslVm(_In_ HANDLE UserToken)
{
    auto lock = m_lock.lock_exclusive();
    THROW_HR_IF(E_CHANGED_STATE, m_shutdown);

    const auto elevated = wsl::windows::common::security::IsTokenElevated(UserToken);
    auto& cachedVm = elevated ? m_adminWslVm : m_wslVm;
    auto& swiotlbConfigured = elevated ? m_adminWslVmSwiotlbConfigured : m_wslVmSwiotlbConfigured;
    if (!cachedVm)
    {
        auto revert = wil::impersonate_token(UserToken);
        const auto& clsid = m_enableTelemetry
                                ? (elevated ? CLSID_WSL_DEVICE_HOST_ADMIN : CLSID_WSL_DEVICE_HOST)
                                : (elevated ? CLSID_WSL_DEVICE_HOST_NO_TELEMETRY_ADMIN : CLSID_WSL_DEVICE_HOST_NO_TELEMETRY);
        const auto host = wil::CoCreateInstance<IWslDeviceHost>(clsid, CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING | CLSCTX_ENABLE_AAA);
        auto vmId = m_runtimeId;
        wil::com_ptr<IWslVm> vm;
        THROW_IF_FAILED(host->OpenVm(&vmId, vm.put()));
        cachedVm = std::move(vm);
    }

    ConfigureSwiotlb(cachedVm, swiotlbConfigured);
    return cachedVm;
}

_Requires_lock_held_(m_lock)
void DeviceHostProxy::ConfigureSwiotlb(const wil::com_ptr<IWslVm>& Vm, bool& Configured)
{
    if (Vm && m_swiotlbConfigured && !Configured)
    {
        THROW_IF_FAILED(Vm->SetSwiotlb(&m_swiotlbConfig));
        Configured = true;
    }
}

wil::com_ptr<IWslDeviceHostCallback> DeviceHostProxy::GetCallback()
{
    wil::com_ptr<IWslDeviceHostCallback> callback;
    THROW_IF_FAILED(CastToUnknown()->QueryInterface(IID_PPV_ARGS(callback.put())));
    return callback;
}

void DeviceHostProxy::TeardownDevice(const wil::com_ptr<IUnknown>& Device) noexcept
{
    if (!Device)
    {
        return;
    }

    if (const auto netDevice = Device.try_query<IWslVirtioNetDevice>())
    {
        LOG_IF_FAILED(netDevice->Teardown());
    }
    else if (const auto virtiofsDevice = Device.try_query<IWslVirtiofsDevice>())
    {
        LOG_IF_FAILED(virtiofsDevice->Teardown());
    }
    else if (const auto pmemDevice = Device.try_query<IWslVirtioPmemDevice>())
    {
        LOG_IF_FAILED(pmemDevice->Teardown());
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

    // Assign the device host process to a fresh kill-on-close job so it is terminated when the VM
    // shuts down. Each process needs its own job: a process the system has already placed in a job
    // cannot be assigned to a job that already owns a different process (ERROR_ACCESS_DENIED).
    {
        auto lock = m_devicesLock.lock_exclusive();
        if (!m_devicesShutdown)
        {
            wil::unique_handle process(OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, ProcessId));
            LOG_LAST_ERROR_IF_MSG(!process, "Failed to open device host process %u for job assignment", ProcessId);
            if (process)
            {
                wil::unique_handle job = wsl::windows::common::helpers::CreateKillOnCloseJob();
                if (AssignProcessToJobObject(job.get(), process.get()))
                {
                    m_processJobs.emplace_back(std::move(job));
                }
                else
                {
                    LOG_LAST_ERROR_MSG("Failed to assign device host process %u to job object", ProcessId);
                }
            }
        }
    }

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
{
    return RegisterDoorbellImpl(InstanceId, BarIndex, Offset, TriggerValue, Flags, Event);
}

HRESULT
DeviceHostProxy::RegisterDoorbell(GUID InstanceId, BYTE BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event)
{
    return RegisterDoorbellImpl(InstanceId, BarIndex, Offset, TriggerValue, Flags, Event);
}

HRESULT DeviceHostProxy::RegisterDoorbellImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags, HANDLE Event) noexcept
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    // Check if the device is one of the known devices that doorbells can be registered for, and
    // if the device has not already registered a doorbell.
    // N.B. For security it is enforced that each device can only register a small number of doorbells.
    //      Currently virtio-9p only uses one and the external virtio device uses two.
    const auto knownDevice = m_devices.find(InstanceId);
    RETURN_HR_IF(E_ACCESSDENIED, knownDevice == m_devices.end() || knownDevice->second.ShuttingDown || knownDevice->second.DoorbellCount == DEVICE_HOST_PROXY_DOORBELL_LIMIT);

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
{
    return UnregisterDoorbellImpl(InstanceId, BarIndex, Offset, TriggerValue, Flags);
}

HRESULT
DeviceHostProxy::UnregisterDoorbell(GUID InstanceId, BYTE BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags)
{
    return UnregisterDoorbellImpl(InstanceId, BarIndex, Offset, TriggerValue, Flags);
}

HRESULT DeviceHostProxy::UnregisterDoorbellImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 Offset, UINT64 TriggerValue, UINT64 Flags) noexcept
try
{
    auto lock = m_devicesLock.lock_exclusive();

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
{
    return CreateSectionBackedMmioRangeImpl(InstanceId, BarIndex, BarOffsetInPages, PageCount, MappingFlags, SectionHandle, SectionOffsetInPages);
}

HRESULT
DeviceHostProxy::CreateSectionBackedMmioRange(
    GUID InstanceId, BYTE BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages)
{
    return CreateSectionBackedMmioRangeImpl(InstanceId, BarIndex, BarOffsetInPages, PageCount, MappingFlags, SectionHandle, SectionOffsetInPages);
}

HRESULT DeviceHostProxy::CreateSectionBackedMmioRangeImpl(
    const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages, UINT64 PageCount, UINT64 MappingFlags, HANDLE SectionHandle, UINT64 SectionOffsetInPages) noexcept
try
{
    auto lock = m_devicesLock.lock_exclusive();
    RETURN_HR_IF(E_CHANGED_STATE, m_devicesShutdown);

    // Check if the device is one of the known devices.
    const auto knownDevice = m_devices.find(InstanceId);
    THROW_HR_IF(E_ACCESSDENIED, knownDevice == m_devices.end() || knownDevice->second.ShuttingDown);

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
{
    return DestroySectionBackedMmioRangeImpl(InstanceId, BarIndex, BarOffsetInPages);
}

HRESULT
DeviceHostProxy::DestroySectionBackedMmioRange(GUID InstanceId, BYTE BarIndex, UINT64 BarOffsetInPages)
{
    return DestroySectionBackedMmioRangeImpl(InstanceId, BarIndex, BarOffsetInPages);
}

HRESULT DeviceHostProxy::DestroySectionBackedMmioRangeImpl(const GUID& InstanceId, UINT8 BarIndex, UINT64 BarOffsetInPages) noexcept
try
{
    auto lock = m_devicesLock.lock_exclusive();
    const auto device = m_devices.find(InstanceId);
    RETURN_HR_IF(E_ACCESSDENIED, device == m_devices.end() || !device->second.MemoryMapping);
    RETURN_IF_FAILED(device->second.MemoryMapping->DestroySectionBackedMmioRange(static_cast<FIOV_BAR_SELECTOR>(BarIndex), BarOffsetInPages));
    return S_OK;
}
CATCH_RETURN()