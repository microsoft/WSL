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
    LinuxDirectBoot,
    LinuxFirmwareBoot,
    NestedVirtualization,
    PerfmonPmu,
    PerfmonLbr,
    SmallPageMemory,
    MemoryOvercommit,
    DeferredMemoryCommit,
    ColdDiscard,
    SerialConsole,
    VirtioConsole,
    Vhd,
    Vhdx,
    PhysicalDisk,
    PersistentMemory,
    Plan9Socket,
    Plan9Virtio,
    VirtioFsFileBacked,
    VirtioFsAggregate,
    SectionBackedSharedMemory,
    MirroredGpu,
    GpuVendorExtension,
    GpuDisableGdiAcceleration,
    GpuDisablePresentation,
    SavedStateOnCrash,
    GuestDmaWindow,
    HostEndpointNetwork,
    UserModeNatNetwork,
    TcpPortBinding,
    UdpPortBinding,
    Ipv6PortBinding,
    ScopedIpv6PortBinding,
    DynamicHostPort,
    VirtualHostAddress,
    StaticDnsARecord,
    Count
};

static_assert(static_cast<size_t>(VmFeature::Count) == 35);

enum class VmOperation
{
    Create,
    Start,
    Terminate,
    CreateGuestListener,
    AcceptGuestConnection,
    ConnectGuest,
    CloseGuestListener,
    AttachDisk,
    DetachDisk,
    AddPersistentMemory,
    CreateFileSystemDevice,
    AddFileSystemShare,
    RemoveFileSystemShare,
    GetFileSystemDeviceStatus,
    AddGpu,
    AddSharedMemory,
    ConfigureGuestDma,
    RemoveDevice,
    AddNetworkAdapter,
    UpdateNetworkAdapter,
    BindPort,
    UnbindPort,
    CreateVirtualAddress,
    CreateDnsRecord,
    Count
};

static_assert(static_cast<size_t>(VmOperation::Count) == 24);

struct VmPlatformCapabilities
{
    BackendKind Backend;
    std::bitset<static_cast<size_t>(VmFeature::Count)> Features;
    std::bitset<static_cast<size_t>(VmOperation::Count)> Operations;
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
};

struct VmCrashCaptureRequest
{
    std::filesystem::path Path;
    std::uint32_t MaxCrashLogCount = 10;
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
    std::optional<std::uint64_t> HighMmioBaseBytes;
    std::optional<std::uint64_t> HighMmioSizeBytes;
};

struct VmEffectiveBoot
{
    VmBootMethod Method = VmBootMethod::Automatic;
    std::wstring KernelCommandLine;
    std::vector<VmConsoleRequest> Consoles;
};

struct VmIpv4Address
{
    std::array<std::uint8_t, 4> Bytes{};
};

struct VmIpv6Address
{
    std::array<std::uint8_t, 16> Bytes{};
    std::uint32_t ScopeId = 0;
};

using VmIpAddress = std::variant<VmIpv4Address, VmIpv6Address>;

struct VmEthernetAddress
{
    std::array<std::uint8_t, 6> Bytes{};
};

struct VmIpEndpoint
{
    VmIpAddress Address;
    std::uint16_t Port = 0;
};

struct VmUserModeNatNetwork
{
    VmIpv4Address ClientIpv4;
    std::optional<VmIpv6Address> ClientIpv6;
    VmEthernetAddress ClientMac;
    VmIpv4Address GatewayIpv4;
    VmEthernetAddress GatewayMacIpv4;
    VmEthernetAddress GatewayMacIpv6;
    VmIpv4Address Netmask;
    std::vector<VmIpAddress> Nameservers;
};

struct VmNetworkAdapterRequest
{
    std::wstring Tag;
    VmUserModeNatNetwork Configuration;
};

struct VmNetworkAttachment
{
    VmDeviceId Id;
    std::wstring Tag;
    std::optional<GUID> GuestInstanceId;
    VmUserModeNatNetwork EffectiveConfiguration;
};

struct VmCreateRequest
{
    VmInstanceId Identity;
    std::wstring Owner;
    VmProcessorRequest Processor;
    VmMemoryRequest Memory;
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

enum class VmTransportProtocol
{
    Tcp,
    Udp
};

struct VmPortBindingRequest
{
    VmTransportProtocol Protocol = VmTransportProtocol::Tcp;
    VmIpEndpoint Listen;
    std::uint16_t GuestPort = 0;
};

struct VmPortBinding
{
    VmPortBindingId Id;
    VmDeviceId Device;
    VmTransportProtocol Protocol = VmTransportProtocol::Tcp;
    VmIpEndpoint EffectiveListen;
    std::uint16_t GuestPort = 0;
};

struct VmDnsRecord
{
    std::string Name;
    VmIpv4Address Address;
};

enum class VmVirtioFsLayout
{
    SingleShare,
    Aggregate
};

struct VmVirtioFsDevice
{
    std::wstring Tag;
    VmVirtioFsLayout Layout = VmVirtioFsLayout::Aggregate;
};

struct VmFileSystemDeviceRequest
{
    VmVirtioFsDevice Transport;
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
};

struct VmVirtioFsShareOptions
{
    std::map<std::wstring, std::wstring> MountOptions;
};

struct VmFileSystemShareRequest
{
    std::filesystem::path HostPath;
    std::wstring Name;
    bool ReadOnly = true;
    VmVirtioFsShareOptions Options;
};

struct VmVirtioFsShareAddress
{
    std::wstring Tag;
    std::optional<std::wstring> ChildName;
};

struct VmFileSystemShare
{
    VmShareId Id;
    VmDeviceId Device;
    VmVirtioFsShareAddress GuestAddress;
    std::filesystem::path EffectiveHostPath;
    bool ReadOnly = true;
};

class IVirtualMachineBackend
{
public:
    using TerminationCallback = std::function<void(GUID)>;

    virtual ~IVirtualMachineBackend() noexcept = default;

    virtual VmPlatformCapabilities GetCapabilities() const = 0;
    virtual VmDescription GetDescription() const = 0;
    virtual wil::unique_handle GetTerminationEvent() const = 0;
    virtual void Start() = 0;
    virtual void Terminate() = 0;
    void RegisterTerminationCallback(TerminationCallback Callback);

    virtual VmGuestListener CreateGuestListener(GuestServicePort Port) = 0;
    virtual wil::unique_socket AcceptGuestConnection(VmListenerId Listener) = 0;
    virtual wil::unique_socket ConnectGuest(GuestServicePort Port) = 0;
    virtual void CloseGuestListener(VmListenerId Listener) = 0;

    virtual VmDiskAttachment AttachDisk(const VmDiskRequest& Request) = 0;
    virtual void DetachDisk(VmDiskId Disk) = 0;

    virtual VmFileSystemDevice CreateFileSystemDevice(const VmFileSystemDeviceRequest& Request) = 0;
    virtual VmFileSystemShare AddFileSystemShare(VmDeviceId Device, const VmFileSystemShareRequest& Request) = 0;
    virtual void RemoveFileSystemShare(VmShareId Share) = 0;

    virtual VmNetworkAttachment AddNetworkAdapter(const VmNetworkAdapterRequest& Request) = 0;
    virtual VmPortBinding BindPort(VmDeviceId Device, const VmPortBindingRequest& Request) = 0;
    virtual void UnbindPort(VmPortBindingId Binding) = 0;

protected:
    // Protects all mutable backend state, including the base listener registry. Callers must
    // release it before performing blocking I/O or waiting for a callback.
    mutable wil::srwlock m_lock;

    // These helpers require m_lock to be held exclusively. ConfigureGuestListener runs while
    // m_lock is held and must not re-enter another listener helper.
    VmGuestListener RegisterGuestListenerLocked(const VmInstanceId& Identity, GuestServicePort Port);
    wil::unique_socket AcceptGuestListenerConnection(VmListenerId Listener, const VmInstanceId& Identity) const;
    std::shared_ptr<VmGuestListenerState> RemoveGuestListenerLocked(VmListenerId Listener, const VmInstanceId& Identity);
    void CloseGuestListenersLocked(const VmInstanceId& Identity) noexcept;
    void NotifyTerminated(const VmInstanceId& Identity) noexcept;

private:
    virtual std::shared_ptr<VmGuestListenerState> ConfigureGuestListener(const VmGuestListener& Listener);

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

bool ValidateFeature(VmFeatureRequest Request, PCWSTR Setting, bool Supported = false);
void ValidateUnsupportedSelection(VmSelectionPolicy Policy);
void ValidatePath(const std::filesystem::path& Path, PCWSTR Backend);
const VmVirtualDiskSource& ValidateDiskRequest(const VmDiskRequest& Request, UINT32 MaximumDisks);
void ValidateConsolePath(const std::filesystem::path& Path, PCWSTR Backend, HRESULT Error, bool RequireName);
void ValidateName(std::wstring_view Name, PCWSTR Description);
void ValidateResourceId(UINT64 Value, const GUID& VmId, const VmInstanceId& Owner);

template <typename Tag>
void ValidateResourceId(const VmResourceId<Tag>& Id, const VmInstanceId& Owner)
{
    ValidateResourceId(Id.Value, Id.Owner.VmId, Owner);
}

} // namespace wsl::windows::common::vm::validation