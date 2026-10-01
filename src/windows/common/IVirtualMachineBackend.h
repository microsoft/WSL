/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IVirtualMachineBackend.h

Abstract:

    Interface for virtual machine backends, providing a common API for managing VMs across different implementations.

--*/

#pragma once

#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <windowsdefs.h>
#include <WslDeviceHost.h>
#include <wil/resource.h>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "defs.h"
#include "stringshared.h"

namespace wsl::windows::common::vm {

inline constexpr UINT64 c_mib = 1024 * 1024;
inline constexpr UINT32 c_maximumDisks = 254;
inline constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

} // namespace wsl::windows::common::vm

enum class BackendKind
{
    Hcs,
    OpenVmm
};

struct VmInstanceId
{
    GUID VmId{};
    wil::shared_handle UserToken{};
};

struct VmGuestListenerState;

template <typename Tag>
struct VmResourceId
{
    VmInstanceId Owner;
    std::uint64_t Value = 0;
};

struct VmDiskTag;
struct VmDeviceTag;
struct VmShareTag;
struct VmListenerTag;
struct VmPortBindingTag;

using VmDiskId = VmResourceId<VmDiskTag>;
using VmDeviceId = VmResourceId<VmDeviceTag>;
using VmShareId = VmResourceId<VmShareTag>;
using VmListenerId = VmResourceId<VmListenerTag>;
using VmPortBindingId = VmResourceId<VmPortBindingTag>;

enum class VmFeatureRequest
{
    Disabled,
    Preferred,
    Required
};

enum class VmSelectionPolicy
{
    Required,
    Preferred
};

template <typename T>
struct VmRequestedValue
{
    T Value{};
    VmSelectionPolicy Policy = VmSelectionPolicy::Required;
};

enum class VmFeature
{
    NestedVirtualization,
    PerfmonPmu,
    PerfmonLbr,
    DeferredMemoryCommit,
    ColdDiscard,
    PhysicalDisk,
    SerialConsole,
    VirtioConsole,
    VirtioFsFileBacked,
    SavedStateOnCrash,
    UserModeNatNetwork,
    TcpPortBinding,
    UdpPortBinding,
    Ipv6PortBinding,
    ScopedIpv6PortBinding,
    Count
};

static_assert(static_cast<size_t>(VmFeature::Count) == 15);

struct VmPlatformCapabilities
{
    BackendKind Backend;
    std::bitset<static_cast<size_t>(VmFeature::Count)> Features;
};

enum class VmState
{
    Created,
    Running,
    Stopped,
    Unknown
};

struct GuestServicePort
{
    std::uint32_t Value = 0;
};

struct VmGuestListener
{
    VmListenerId Id;
    GuestServicePort Port;
    std::shared_ptr<VmGuestListenerState> State;

    wil::unique_socket Accept() const;
};

struct VmGuestListenerState
{
    VmGuestListener Listener;
    wil::unique_socket Socket;
    wil::unique_event CancellationEvent{wil::EventOptions::ManualReset};
};

struct VmProcessorRequest
{
    std::uint32_t Count = 0;
    VmFeatureRequest NestedVirtualization = VmFeatureRequest::Disabled;
    VmFeatureRequest PerfmonPmu = VmFeatureRequest::Disabled;
    VmFeatureRequest PerfmonLbr = VmFeatureRequest::Disabled;
};

struct VmMmioRequest
{
    std::uint64_t HighWindowSizeBytes = 0;
    std::optional<std::uint8_t> MaximumGuestAddressBits;
};

struct VmMemoryRequest
{
    std::uint64_t SizeBytes = 0;
    VmFeatureRequest AllowOvercommit = VmFeatureRequest::Disabled;
    VmFeatureRequest DeferredCommit = VmFeatureRequest::Disabled;
    VmFeatureRequest ColdDiscard = VmFeatureRequest::Disabled;
    VmFeatureRequest SmallPageBacking = VmFeatureRequest::Disabled;
    std::optional<std::uint32_t> FaultClusterSizeShift;
    std::optional<std::uint32_t> DirectMapFaultClusterSizeShift;
    std::optional<std::uint32_t> PageReportingOrder;
    std::optional<std::wstring> HostingProcessNameSuffix;
};

enum class VmBootMethod
{
    Automatic,
    LinuxDirect,
    Uefi
};

struct VmLinuxBootRequest
{
    std::filesystem::path KernelPath;
    std::filesystem::path InitrdPath;
    VmBootMethod Method = VmBootMethod::Automatic;
    std::wstring KernelCommandLine;
};

enum class VmConsoleRole
{
    EarlyBoot,
    KernelConsole,
    Telemetry,
    DebugShell,
    KernelDebugger
};

struct VmSerialConsole
{
    std::uint32_t Port = 0;
    std::filesystem::path NamedPipe;
};

struct VmVirtioConsole
{
    std::uint32_t Port = 0;
    std::wstring GuestName;
    std::filesystem::path NamedPipe;
};

struct VmConsoleRequest
{
    VmConsoleRole Role = VmConsoleRole::KernelConsole;
    std::variant<VmSerialConsole, VmVirtioConsole> Device;
};

using VmBootResourceKey = std::wstring;

struct VmScsiAddress
{
    std::uint32_t Controller = 0;
    std::uint32_t Lun = 0;
};

using VmGuestDiskAddress = VmScsiAddress;

enum class VmDiskFormat
{
    Vhd,
    Vhdx
};

struct VmVirtualDiskSource
{
    std::filesystem::path Path;
    VmDiskFormat Format = VmDiskFormat::Vhdx;
};

struct VmPhysicalDiskSource
{
    std::wstring DevicePath;
};

struct VmScsiPlacement
{
    VmScsiAddress Address;
};

struct VmDiskRequest
{
    std::variant<VmVirtualDiskSource, VmPhysicalDiskSource> Source;
    bool ReadOnly = true;
    std::optional<VmScsiPlacement> Placement;
    // Set for disks that the user explicitly attached (for instance via 'wsl --mount'), as opposed
    // to disks that WSL attaches on the user's behalf.
    bool UserDisk = false;
    // Timeout applied to host disk state changes and to retries when attaching a physical disk.
    std::chrono::milliseconds DeviceTimeout{5000};
    wil::shared_handle UserToken{};
};

struct VmBootDiskRequest
{
    VmBootResourceKey Key;
    VmDiskRequest Disk;
};

struct VmDiskAttachment
{
    VmDiskId Id;
    VmGuestDiskAddress GuestAddress;
    bool ReadOnly = true;
    bool UserDisk = false;
};

struct VmCrashCaptureRequest
{
    std::filesystem::path Path;
    std::uint32_t MaxCrashLogCount = 10;
    std::optional<std::filesystem::path> SavedStateFolder;
    std::uint32_t MaxSavedStateCount = 10;
    VmSelectionPolicy Policy = VmSelectionPolicy::Required;
};

struct VmEffectiveProcessor
{
    std::uint32_t Count = 0;
    bool NestedVirtualization = false;
    bool PerfmonPmu = false;
    bool PerfmonLbr = false;
};

struct VmEffectiveMemory
{
    std::uint64_t SizeBytes = 0;
    bool AllowOvercommit = false;
    bool DeferredCommit = false;
    bool ColdDiscard = false;
    bool SmallPageBacking = false;
    std::optional<std::uint32_t> FaultClusterSizeShift;
    std::optional<std::uint32_t> DirectMapFaultClusterSizeShift;
    std::optional<std::uint32_t> PageReportingOrder;
    std::optional<std::wstring> HostingProcessNameSuffix;
    std::optional<std::uint64_t> HighMmioBaseBytes;
    std::optional<std::uint64_t> HighMmioSizeBytes;
};

struct VmEffectiveBoot
{
    VmBootMethod Method = VmBootMethod::Automatic;
    std::wstring KernelCommandLine;
    std::vector<VmConsoleRequest> Consoles;
};

// Network served by a user-mode NAT that runs on the host and reaches the guest through a virtio-net
// device. The device configuration is described with the guest device host ABI types so that the
// NAT's view of the guest is not restated by every backend.
struct VmUserModeNatNetwork
{
    WslVirtioNetConfig Configuration{};
    std::vector<IpAddress> Nameservers;

    wsl::shared::string::MacAddress ClientMacAddress() const;
};

// Network served by an endpoint that the caller created on the host network stack. The mirrored and
// NAT networking modes use this: the host owns the network, address assignment and name resolution,
// so the backend only has to attach the endpoint to the VM as an adapter.
struct VmHostEndpointNetwork
{
    GUID EndpointId{};
    // Identifies the adapter inside the VM. Mirrored networking sets this to the interface id of the
    // host interface being mirrored so that an interface keeps the same adapter as its endpoint is
    // added and removed; NAT has no host interface to match and reuses the endpoint id.
    GUID InstanceId{};
    wsl::shared::string::MacAddress MacAddress{};
};

using VmNetworkConfiguration = std::variant<VmHostEndpointNetwork, VmUserModeNatNetwork>;

struct VmNetworkAdapterRequest
{
    std::wstring Tag;
    VmNetworkConfiguration Configuration;
};

struct VmNetworkAttachment
{
    VmDeviceId Id;
    std::wstring Tag;
    std::optional<GUID> GuestInstanceId;
    VmNetworkConfiguration EffectiveConfiguration;
};

struct VmCreateRequest
{
    VmInstanceId Identity;
    std::wstring Owner;
    VmProcessorRequest Processor;
    VmMemoryRequest Memory;
    VmMmioRequest Mmio;
    VmLinuxBootRequest Boot;
    std::vector<VmBootDiskRequest> BootDisks;
    std::vector<VmConsoleRequest> Consoles;
    std::optional<VmCrashCaptureRequest> CrashCapture;
    std::vector<VmNetworkAdapterRequest> NetworkAdapters;
};

struct VmDescription
{
    VmInstanceId Identity;
    BackendKind Backend;
    VmEffectiveProcessor Processor;
    VmEffectiveMemory Memory;
    VmEffectiveBoot Boot;
    std::map<VmBootResourceKey, VmDiskAttachment> BootDisks;
    std::map<std::wstring, VmNetworkAttachment> NetworkAdapters;
};

struct VmPortBindingRequest
{
    TransportProtocol Protocol = TransportProtocol_Tcp;
    // Host endpoint to listen on. A zero HostPort asks the backend to allocate one.
    IpAddress ListenAddress;
    std::uint32_t ListenScopeId = 0;
    std::uint16_t HostPort = 0;
    std::uint16_t GuestPort = 0;
};

struct VmPortBinding
{
    VmPortBindingId Id;
    VmDeviceId Device;
    TransportProtocol Protocol = TransportProtocol_Tcp;
    IpAddress EffectiveListenAddress;
    std::uint32_t EffectiveListenScopeId = 0;
    std::uint16_t EffectiveHostPort = 0;
    std::uint16_t GuestPort = 0;
};

struct VmDnsRecord
{
    DnsRecordType Type = DnsRecordType_A;
    // Name the guest resolves and the address returned for it.
    std::string Name;
    IpAddress Address;
};

enum class VmVirtioFsLayout
{
    SingleShare,
    Aggregate
};

// The guest identifies a virtio-fs device by its tag, which cannot exceed the length of a GUID
// without braces.
constexpr std::size_t c_maxVirtioFsTagLength = 36;

struct VmVirtioFsShareOptions
{
    std::map<std::wstring, std::wstring> MountOptions;
};

struct VmVirtioFsDevice
{
    std::wstring Tag;
    VmVirtioFsLayout Layout = VmVirtioFsLayout::Aggregate;
    // Mount options applied to the device itself. Shares of an aggregate device carry their own
    // options; a single-share device inherits the options of the share that it serves.
    VmVirtioFsShareOptions Options;
};

using VmPlan9ServerFactory = std::function<wil::com_ptr<IPlan9FileSystem>(HANDLE UserToken)>;

struct VmPlan9SocketDevice
{
    GuestServicePort Port;
    VmPlan9ServerFactory ServerFactory;
};

// Plan 9 server hosted directly by HCS rather than by an out-of-process IPlan9FileSystem server.
// This is used for shares that must remain available independently of DrvFs.
struct VmPlan9HostedDevice
{
    GuestServicePort Port;
};

struct VmPlan9VirtioDevice
{
    std::wstring Tag;
    // Used to register a virtio server with the guest device host.
    GUID FileSystemClassId{};
    // Used to create the initial virtio device for the server.
    GUID DeviceType{};
    VmPlan9ServerFactory ServerFactory;
};

using VmFileSystemDeviceTransport =
    std::variant<VmVirtioFsDevice, VmPlan9SocketDevice, VmPlan9HostedDevice, VmPlan9VirtioDevice>;

struct VmFileSystemDeviceRequest
{
    VmFileSystemDeviceTransport Transport;
};

enum class VmFileSystemDeviceState
{
    Prepared,
    Serving,
    Unavailable
};

struct VmFileSystemDevice
{
    VmDeviceId Id;
    VmFileSystemDeviceState State = VmFileSystemDeviceState::Prepared;
    // Set once the device exists in the VM. Backends that create the device when its first share is
    // added report a prepared device without a guest instance id.
    std::optional<GUID> GuestInstanceId;
};

struct VmPlan9ShareOptions
{
    bool LinuxMetadata = false;
    bool CaseSensitive = false;
    bool UseShareRootIdentity = false;
    bool AllowOptions = false;
    bool AllowSubPaths = false;
};

using VmFileSystemShareOptions = std::variant<VmVirtioFsShareOptions, VmPlan9ShareOptions>;

struct VmFileSystemShareRequest
{
    std::filesystem::path HostPath;
    // Child name for aggregate virtio-fs devices or access name for Plan9 shares. Generated when empty.
    std::wstring Name;
    bool ReadOnly = true;
    VmFileSystemShareOptions Options;
    // Token whose identity is used to reach the host path. Elevated and unelevated callers share the
    // same VM, so a share carries its own token instead of reusing the one that created the VM. The
    // VM identity token is used when this is unset.
    wil::shared_handle UserToken{};
};

struct VmVirtioFsShareAddress
{
    std::wstring Tag;
    std::optional<std::wstring> ChildName;
};

struct VmPlan9SocketShareAddress
{
    GuestServicePort Port;
    std::wstring AccessName;
};

struct VmPlan9VirtioShareAddress
{
    std::wstring Tag;
    std::wstring AccessName;
};

using VmFileSystemShareAddress = std::variant<VmVirtioFsShareAddress, VmPlan9SocketShareAddress, VmPlan9VirtioShareAddress>;

struct VmFileSystemShare
{
    VmShareId Id;
    VmDeviceId Device;
    VmFileSystemShareAddress GuestAddress;
    std::filesystem::path EffectiveHostPath;
    bool ReadOnly = true;
};

// Invoked with the index of a newly added persistent memory device while the backend still
// serializes persistent memory additions. Callers that name devices after the order in which the
// guest enumerated them (/dev/pmem<index>) use this to wait for the device to appear before the
// next one is added. The device is already added and tracked when this runs, so throwing fails
// AddPersistentMemory without reusing the device's index.
using VmPersistentMemoryReadyCallback = std::function<void(std::uint32_t DeviceIndex)>;

struct VmPersistentMemoryRequest
{
    std::filesystem::path Path;
    bool ReadOnly = true;
    // Token whose identity is used to reach the backing file. The VM identity token is used when
    // this is unset.
    wil::shared_handle UserToken{};
    VmPersistentMemoryReadyCallback WaitForGuestDevice;
};

struct VmPersistentMemoryDevice
{
    VmDeviceId Id;
    GUID GuestInstanceId{};
    // Position of the device among the persistent memory devices added to this VM. The guest names
    // the device after the order in which it enumerated it, so this matches /dev/pmem<Index> as
    // long as every device is added through the backend.
    std::uint32_t Index = 0;
    std::filesystem::path EffectiveHostPath;
    bool ReadOnly = true;
};

enum class VmGpuAssignmentMode
{
    Mirror
};

struct VmGpuRequest
{
    VmGpuAssignmentMode AssignmentMode = VmGpuAssignmentMode::Mirror;
    VmFeatureRequest VendorExtension = VmFeatureRequest::Preferred;
    // Presentation and GDI acceleration are only controllable on hosts that support the setting; a
    // preferred request leaves them at the host default elsewhere.
    VmFeatureRequest DisableGdiAcceleration = VmFeatureRequest::Preferred;
    VmFeatureRequest DisablePresentation = VmFeatureRequest::Preferred;
};

struct VmGpuAttachment
{
    VmDeviceId Id;
    VmGpuAssignmentMode AssignmentMode = VmGpuAssignmentMode::Mirror;
    bool VendorExtension = false;
    bool GdiAccelerationDisabled = false;
    bool PresentationDisabled = false;
};

struct VmSharedMemoryRequest
{
    std::wstring Tag;
    std::wstring Path;
    std::uint64_t SizeBytes = 0;
    wil::shared_handle UserToken{};
};

struct VmSharedMemoryDevice
{
    VmDeviceId Id;
    GUID GuestInstanceId{};
    std::wstring Tag;
    std::wstring ObjectPath;
    std::uint64_t SizeBytes = 0;
};

struct VmGuestDmaRequest
{
    std::uint64_t BaseAddress = 0;
    std::uint64_t SizeBytes = 0;
};

struct VmCrashInformation
{
    bool Crashed = false;
    std::optional<std::filesystem::path> CrashLogFile;
    std::optional<std::filesystem::path> SavedStateFile;
};

class IVirtualMachineBackend
{
public:
    using TerminationCallback = std::function<void(GUID)>;

    virtual ~IVirtualMachineBackend() noexcept = default;

    virtual VmPlatformCapabilities GetCapabilities() const = 0;
    // Returns the effective creation-time configuration, including IDs used for boot resource operations.
    virtual VmDescription GetDescription() const = 0;
    virtual VmState GetState() const = 0;
    virtual std::wstring GetExitDetails() const = 0;
    virtual VmCrashInformation GetCrashInformation() const = 0;
    virtual wil::unique_handle GetTerminationEvent() const = 0;
    virtual void Start() = 0;
    virtual void Terminate() = 0;
    void RegisterTerminationCallback(TerminationCallback Callback);

    virtual VmGuestListener CreateGuestListener(GuestServicePort Port) = 0;
    virtual wil::unique_socket ConnectGuest(GuestServicePort Port) = 0;
    virtual void CloseGuestListener(VmListenerId Listener) = 0;

    virtual VmDiskAttachment AttachDisk(const VmDiskRequest& Request) = 0;
    virtual void DetachDisk(VmDiskId Disk) = 0;

    /// <summary>
    /// Exposes a host file to the guest as a persistent memory device. Additions are serialized so
    /// that devices are enumerated by the guest in the order they were added.
    /// </summary>
    virtual VmPersistentMemoryDevice AddPersistentMemory(const VmPersistentMemoryRequest& Request) = 0;

    /// <summary>
    /// Assigns the host GPUs to the VM and reports the settings that were applied.
    /// </summary>
    virtual VmGpuAttachment AddGpu(const VmGpuRequest& Request) = 0;

    virtual VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) = 0;
    virtual VmFileSystemDevice GetFileSystemDeviceStatus(VmDeviceId Device) = 0;
    virtual VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) = 0;
    virtual void RemoveFileSystemShare(VmShareId Share) = 0;
    virtual VmSharedMemoryDevice AddSharedMemory(const VmSharedMemoryRequest& Request) = 0;
    virtual void ConfigureGuestDma(const VmGuestDmaRequest& Request) = 0;
    virtual void RemoveDevice(VmDeviceId Device) = 0;

    virtual VmNetworkAttachment AddNetworkAdapter(const VmNetworkAdapterRequest& Request) = 0;
    virtual void UpdateNetworkAdapter(VmDeviceId Device, const VmNetworkConfiguration& Configuration) = 0;
    virtual void RemoveNetworkAdapter(VmDeviceId Device) = 0;
    virtual VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) = 0;
    virtual void UnbindPort(VmPortBindingId Binding) = 0;
    virtual IpAddress CreateVirtualAddress(VmDeviceId Device, const IpAddress& Destination) = 0;
    virtual void CreateDnsRecord(VmDeviceId Device, const VmDnsRecord& Record) = 0;

protected:
    // Protects all mutable backend state, including the base listener registry. Callers must
    // release it before performing blocking I/O or waiting for a callback.
    mutable wil::srwlock m_lock;

    // These helpers require m_lock to be held exclusively. ConfigureGuestListener runs while
    // m_lock is held and must not re-enter another listener helper.
    VmGuestListener RegisterGuestListenerLocked(const VmInstanceId& Identity, GuestServicePort Port);
    std::shared_ptr<VmGuestListenerState> RemoveGuestListenerLocked(VmListenerId Listener, const VmInstanceId& Identity);
    void CloseGuestListenersLocked(const VmInstanceId& Identity) noexcept;
    void NotifyTerminated(const VmInstanceId& Identity) noexcept;

private:
    virtual std::shared_ptr<VmGuestListenerState> ConfigureGuestListener(const VmGuestListener& Listener) = 0;

    _Guarded_by_(m_lock) std::map<std::uint64_t, std::shared_ptr<VmGuestListenerState>> m_guestListeners;
    _Guarded_by_(m_lock) std::uint64_t m_nextListenerId = 1;
    wil::srwlock m_terminationCallbackLock;
    _Guarded_by_(m_terminationCallbackLock) bool m_terminated = false;
    _Guarded_by_(m_terminationCallbackLock) GUID m_terminatedVmId {};
    _Guarded_by_(m_terminationCallbackLock) TerminationCallback m_terminationCallback;
};

VmPlatformCapabilities QueryVirtualMachineBackendCapabilities(BackendKind Kind);

std::unique_ptr<IVirtualMachineBackend> CreateVirtualMachineBackend(BackendKind Kind, const VmCreateRequest& Request);

namespace wsl::windows::common::vm::validation {

/// <summary>
/// Validates that a resource id was issued by the backend that owns the VM it is used with.
/// </summary>
template <typename Tag>
void ValidateResourceId(const VmResourceId<Tag>& Id, const VmInstanceId& Owner)
{
    THROW_HR_IF(E_INVALIDARG, Id.Value == 0 || !IsEqualGUID(Id.Owner.VmId, Owner.VmId));
}

bool ValidateFeature(VmFeatureRequest Request, PCWSTR Setting, bool Supported = false);

void ValidateDiskPlacement(const VmDiskRequest& Request);

const VmVirtualDiskSource& ValidateDiskRequest(const VmDiskRequest& Request);

const std::wstring& ValidateDiskSource(const VmDiskRequest& Request);

} // namespace wsl::windows::common::vm::validation