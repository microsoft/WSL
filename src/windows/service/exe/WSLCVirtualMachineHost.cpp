/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVirtualMachineHost.cpp

Abstract:

    Implementation of IWSLCVirtualMachine - the service-side WSLC VM host.

--*/

#include "WSLCVirtualMachineHost.h"
#include <string>
#include <string_view>
#include "wslsecurity.h"
#include "wslutil.h"
#include "lxinitshared.h"
#include "DnsResolver.h"
#include "string.hpp"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using wsl::windows::service::wslc::WSLCVirtualMachineHost;
namespace {

SOCKADDR_INET CreateListenAddress(LPCSTR Address, uint16_t HostPort)
{
    auto listenAddr = wsl::windows::common::string::StringToSockAddrInet(wsl::shared::string::MultiByteToWide(Address));

    if (listenAddr.si_family == AF_INET)
    {
        listenAddr.Ipv4.sin_port = HostPort;
    }
    else if (listenAddr.si_family == AF_INET6)
    {
        listenAddr.Ipv6.sin6_port = HostPort;
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Unsupported address family: %d", listenAddr.si_family);
    }

    return listenAddr;
}

// Replace any character outside the conservative ASCII allowlist with '_'.
std::wstring SanitizeHostingProcessNameSuffix(std::wstring_view name)
{
    constexpr std::wstring_view c_allowed = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::wstring sanitized{name};
    for (auto& c : sanitized)
    {
        if (c_allowed.find(c) == std::wstring_view::npos)
        {
            c = L'_';
        }
    }

    return sanitized;
}

} // namespace

WSLCVirtualMachineHost::WSLCVirtualMachineHost(_In_ const WSLCSessionSettings* Settings, WSLCVirtualMachineBackendFactory BackendFactory)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);
    THROW_HR_IF(E_INVALIDARG, !BackendFactory);

    const auto userToken = wil::shared_handle{wsl::windows::common::security::GetUserToken(TokenImpersonation).release()};
    std::lock_guard lock(m_lock);

    GUID vmId{};
    THROW_IF_FAILED(CoCreateGuid(&vmId));
    m_featureFlags = Settings->FeatureFlags;
    m_networkingMode = Settings->NetworkingMode;
    m_hostLoopback = Settings->HostLoopback ? Settings->HostLoopback : "";
    m_bootTimeoutMs = Settings->BootTimeoutMs;

    VmCreateRequest request{};
    request.Identity = {vmId, userToken};
    request.Owner = Settings->DisplayName ? Settings->DisplayName : L"WSLC";
    request.Processor.Count = Settings->CpuCount;
    request.Processor.NestedVirtualization = VmFeatureRequest::Preferred;
    request.Processor.PerfmonPmu = VmFeatureRequest::Preferred;
    request.Processor.PerfmonLbr = VmFeatureRequest::Preferred;
    request.Memory.SizeBytes = static_cast<UINT64>(Settings->MemoryMb & ~0x1UL) * _1MB;
    request.Memory.AllowOvercommit = VmFeatureRequest::Required;
    request.Memory.DeferredCommit = VmFeatureRequest::Required;
    request.Memory.ColdDiscard = VmFeatureRequest::Required;
    request.Memory.SmallPageBacking = VmFeatureRequest::Preferred;
    request.Memory.HostingProcessNameSuffix =
        Settings->DisplayName ? std::optional{SanitizeHostingProcessNameSuffix(Settings->DisplayName)} : std::nullopt;

    const auto basePath = wslutil::GetBasePath();
#ifdef WSL_KERNEL_PATH
    request.Boot.KernelPath = WSL_KERNEL_PATH;
#else
    request.Boot.KernelPath = basePath / L"tools" / LXSS_VM_MODE_KERNEL_NAME;
#endif
    request.Boot.Method = wsl::shared::Arm64 ? VmBootMethod::Uefi : VmBootMethod::LinuxDirect;
    if constexpr (!wsl::shared::Arm64)
    {
        request.Boot.InitrdPath = basePath / L"tools" / LXSS_VM_MODE_INITRD_NAME;
    }

    const auto pageReportingOrder =
        wsl::windows::common::helpers::GetWindowsVersion().BuildNumber >= WindowsBuildNumbers::Germanium ? 5 : 9;
    auto swiotlbSizeBytes = (FeatureEnabled(WslcFeatureFlagsVirtioFs) || m_networkingMode == WSLCNetworkingModeConsomme)
                                ? helpers::ComputeDefaultSwiotlbConfig(static_cast<UINT64>(Settings->MemoryMb) * _1MB)
                                : 0;
    request.Boot.KernelCommandLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLC_ROOT_INIT_ENV) L"=1 panic=-1";
    helpers::AppendCommonKernelCommandLine(request.Boot.KernelCommandLine, pageReportingOrder, swiotlbSizeBytes, Settings->CpuCount);

    wil::unique_handle dmesgOutputHandle;
    if (Settings->DmesgOutput.Handle.File != nullptr && Settings->DmesgOutput.Handle.File != INVALID_HANDLE_VALUE)
    {
        dmesgOutputHandle.reset(wslutil::DuplicateHandle(wslutil::FromCOMInputHandle(Settings->DmesgOutput), GENERIC_WRITE | SYNCHRONIZE));
    }

    m_dmesgCollector = DmesgCollector::Create(
        vmId, m_vmExitEvent.get(), true, false, L"", FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg), std::move(dmesgOutputHandle));
    if (FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg))
    {
        request.Boot.KernelCommandLine += wsl::shared::Arm64 ? L" earlycon=pl011,0xeffec000,115200" : L" earlycon=uart8250,io,0x3f8,115200";
        request.Consoles.push_back({VmConsoleRole::EarlyBoot, VmSerialConsole{0, m_dmesgCollector->EarlyConsoleName()}});
    }
    if (helpers::IsVirtioSerialConsoleSupported())
    {
        request.Boot.KernelCommandLine += L" console=hvc0 debug";
        request.Consoles.push_back({VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"hvc0", m_dmesgCollector->VirtioConsoleName()}});
    }

#ifdef WSL_KERNEL_MODULES_PATH
    const auto kernelModulesPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));
#else
    const auto kernelModulesPath = basePath / L"tools" / L"artifacts.vhd";
#endif
    std::filesystem::path rootVhdPath = Settings->RootVhdOverride ? Settings->RootVhdOverride
#ifdef WSL_SYSTEM_DISTRO_PATH
                                                                  : std::filesystem::path(TEXT(WSL_SYSTEM_DISTRO_PATH));
#else
                                                                  : std::filesystem::path(wslutil::GetMsiPackagePath().value()) /
                                                                        L"system.vhd";
#endif

    auto addBootDisk = [&](std::wstring key, const std::filesystem::path& path, bool grantHostAccess) {
        VmBootDiskRequest disk{};
        disk.Key = std::move(key);
        disk.Disk.Source = VmVirtualDiskSource{path, VmDiskFormat::Vhd};
        disk.Disk.ReadOnly = true;
        disk.GrantHostAccess = grantHostAccess;
        request.BootDisks.emplace_back(std::move(disk));
    };
    addBootDisk(L"root", rootVhdPath, Settings->RootVhdOverride != nullptr);
    addBootDisk(L"kernel-modules", kernelModulesPath, false);

    auto resources = BackendFactory(request);
    m_backend = std::move(resources.Backend);
    m_networkingFactory = std::move(resources.NetworkingFactory);
    THROW_IF_NULL_ALLOC(m_backend);
    THROW_HR_IF(E_INVALIDARG, !m_networkingFactory);
    m_backend->RegisterTerminationCallback([this](GUID) { m_vmExitEvent.SetEvent(); });

    m_guestListener = m_backend->CreateGuestListener(GuestServicePort{LX_INIT_UTILITY_VM_INIT_PORT});
    m_backend->Start();
    if (FeatureEnabled(WslcFeatureFlagsGPU))
    {
        m_backend->AddGpu({});
    }
}

WSLCVirtualMachineHost::~WSLCVirtualMachineHost()
{
    m_backend.reset();
}

bool WSLCVirtualMachineHost::FeatureEnabled(WSLCFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

HRESULT WSLCVirtualMachineHost::GetId(_Out_ GUID* VmId)
try
{
    RETURN_HR_IF_NULL(E_POINTER, VmId);

    *VmId = m_backend->GetDescription().Identity.VmId;
    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::AcceptConnection(_Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Socket);

    *Socket = reinterpret_cast<HANDLE>(m_guestListener.Accept(m_bootTimeoutMs).release());
    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_networking.has_value());

    if (m_networkingMode == WSLCNetworkingModeNone)
    {
        return S_OK;
    }

    // Duplicate the socket handles - COM manages the lifetime of the marshalled handles,
    // so we need our own copies to take ownership.
    wil::unique_socket gnsSocketHandle{reinterpret_cast<SOCKET>(wslutil::DuplicateHandle(GnsSocket))};
    wil::unique_socket dnsSocketHandle;

    // The DNS hvsocket is only allocated for NAT mode.
    THROW_HR_IF(E_INVALIDARG, (FeatureEnabled(WslcFeatureFlagsDnsTunneling) && m_networkingMode == WSLCNetworkingModeNAT) != (DnsSocket != nullptr));

    // The check still applies to Consomme because the host Consomme NAT uses the same Windows DNS APIs.
    if (FeatureEnabled(WslcFeatureFlagsDnsTunneling))
    {
        const auto result = wsl::core::networking::DnsResolver::LoadDnsResolverMethods();
        if (FAILED(result))
        {
            LOG_HR_MSG(result, "Failed to load DNS resolver methods, DNS tunneling will be disabled");
            WI_ClearFlag(m_featureFlags, WslcFeatureFlagsDnsTunneling);
        }
    }

    if (DnsSocket != nullptr && FeatureEnabled(WslcFeatureFlagsDnsTunneling))
    {
        dnsSocketHandle.reset(reinterpret_cast<SOCKET>(wslutil::DuplicateHandle(*DnsSocket)));
    }

    const auto description = m_backend->GetDescription();
    if (m_networkingMode == WSLCNetworkingModeNAT)
    {
        WSLCNetworkingRequest request{};
        request.Mode = WSLCNetworkingModeNAT;
        request.EnableDnsTunneling = FeatureEnabled(WslcFeatureFlagsDnsTunneling);
        m_networking = m_networkingFactory(
            *m_backend,
            request,
            wsl::core::GnsChannel(std::move(gnsSocketHandle)),
            std::move(dnsSocketHandle),
            description.Identity.UserToken.get());
    }
    else if (m_networkingMode == WSLCNetworkingModeConsomme)
    {
        WSLCNetworkingRequest request{};
        request.Mode = WSLCNetworkingModeConsomme;
        request.EnableDnsTunneling = FeatureEnabled(WslcFeatureFlagsDnsTunneling);
        request.EnableLocalhostRelay = !FeatureEnabled(WslcFeatureFlagsPortRelayWslRelay);
        request.HostLoopback = m_hostLoopback;
        m_networking = m_networkingFactory(
            *m_backend,
            request,
            wsl::core::GnsChannel(std::move(gnsSocketHandle)),
            std::move(dnsSocketHandle),
            description.Identity.UserToken.get());
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %lu", m_networkingMode);
    }

    THROW_IF_NULL_ALLOC(m_networking->Engine);
    m_networking->Engine->Initialize();

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::AttachDisk(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun)
try
{
    RETURN_HR_IF(E_POINTER, Path == nullptr || Lun == nullptr);

    std::lock_guard lock(m_lock);
    VmDiskRequest request{};
    const std::filesystem::path path{Path};
    VmDiskFormat format;
    if (wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), wsl::windows::common::wslutil::c_vhdFileExtension))
    {
        format = VmDiskFormat::Vhd;
    }
    else
    {
        THROW_HR_IF(E_INVALIDARG, !wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), wsl::windows::common::wslutil::c_vhdxFileExtension));
        format = VmDiskFormat::Vhdx;
    }
    request.Source = VmVirtualDiskSource{path, format};
    request.ReadOnly = ReadOnly;
    request.UserDisk = true;
    request.BootCritical = true;
    const auto attachment = m_backend->AttachDisk(request);
    *Lun = attachment.GuestAddress.Lun;
    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock(m_lock);

    const auto disks = m_backend->GetAttachedDisks();
    const auto disk = std::ranges::find_if(
        disks, [&](const VmDiskAttachment& candidate) { return candidate.UserDisk && candidate.GuestAddress.Lun == Lun; });
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), disk == disks.end());
    m_backend->DetachDisk(disk->Id);

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::AddShare(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || ShareId == nullptr);

    std::lock_guard lock(m_lock);

    GUID shareIdLocal;
    THROW_IF_FAILED(CoCreateGuid(&shareIdLocal));
    auto shareName = wsl::shared::string::GuidToString<wchar_t>(shareIdLocal, wsl::shared::string::None);

    const auto userToken = m_backend->GetDescription().Identity.UserToken;

    if (!FeatureEnabled(WslcFeatureFlagsVirtioFs))
    {
        auto device = m_backend->GetFileSystemDevice([](const VmFileSystemDevice& candidate) {
            const auto* transport = std::get_if<VmPlan9SocketDevice>(&candidate.Transport);
            return transport != nullptr && transport->Port.Value == LX_INIT_UTILITY_VM_PLAN9_PORT;
        });
        if (!device)
        {
            VmFileSystemDeviceRequest deviceRequest{};
            deviceRequest.UserToken = wil::shared_handle{wslutil::DuplicateHandle(userToken.get())};
            deviceRequest.Transport =
                VmPlan9SocketDevice{GuestServicePort{LX_INIT_UTILITY_VM_PLAN9_PORT}, [](HANDLE userToken) {
                                        return wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(userToken);
                                    }};
            device = m_backend->CreateFileSystemDevice(deviceRequest);
        }

        VmFileSystemShareRequest shareRequest{};
        shareRequest.HostPath = WindowsPath;
        shareRequest.Name = shareName;
        shareRequest.ReadOnly = ReadOnly;
        shareRequest.Options = VmPlan9ShareOptions{false, false, false, true, false};
        shareRequest.UserToken = wil::shared_handle{wslutil::DuplicateHandle(userToken.get())};
        m_backend->AddFileSystemShare(device->Id, shareRequest);
    }
    else
    {
        auto device = m_backend->GetFileSystemDevice([](const VmFileSystemDevice& candidate) {
            const auto* transport = std::get_if<VmVirtioFsDevice>(&candidate.Transport);
            return transport != nullptr && transport->Layout == VmVirtioFsLayout::Aggregate && transport->Tag == TEXT(LX_INIT_DRVFS_VIRTIO_TAG);
        });
        if (!device)
        {
            VmFileSystemDeviceRequest deviceRequest{};
            deviceRequest.UserToken = wil::shared_handle{wslutil::DuplicateHandle(userToken.get())};
            deviceRequest.Transport = VmVirtioFsDevice{TEXT(LX_INIT_DRVFS_VIRTIO_TAG), VmVirtioFsLayout::Aggregate};
            device = m_backend->CreateFileSystemDevice(deviceRequest);
        }

        VmFileSystemShareRequest shareRequest{};
        shareRequest.HostPath = WindowsPath;
        shareRequest.Name = shareName;
        shareRequest.ReadOnly = ReadOnly;
        shareRequest.Options = VmVirtioFsShareOptions{.MountOptions = {{L"metadata", L""}}};
        m_backend->AddFileSystemShare(device->Id, shareRequest);
    }

    *ShareId = shareIdLocal;
    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::RemoveShare(_In_ REFGUID ShareId)
try
{
    std::lock_guard lock(m_lock);

    const auto shareName = wsl::shared::string::GuidToString<wchar_t>(ShareId, wsl::shared::string::None);
    const auto share = m_backend->GetFileSystemShare([&](const VmFileSystemShare& candidate) {
        if (const auto* address = std::get_if<VmPlan9SocketShareAddress>(&candidate.GuestAddress))
        {
            return address->Port.Value == LX_INIT_UTILITY_VM_PLAN9_PORT && address->AccessName == shareName;
        }

        if (const auto* address = std::get_if<VmVirtioFsShareAddress>(&candidate.GuestAddress))
        {
            return address->Tag == TEXT(LX_INIT_DRVFS_VIRTIO_TAG) && address->ChildName == shareName;
        }

        return false;
    });
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !share);
    m_backend->RemoveFileSystemShare(share->Id);

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::ApplyGuestCapabilities(_In_ const WSLCGuestCapabilities* Capabilities)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Capabilities);

    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_INVALIDARG, m_swiotlbConfigured);

    if (Capabilities->HvPciSwiotlbBase != 0 && Capabilities->HvPciSwiotlbSize != 0)
    {
        if (m_backend->GetCapabilities().Features.test(static_cast<size_t>(VmFeature::GuestDmaWindow)))
        {
            m_backend->ConfigureGuestDma({Capabilities->HvPciSwiotlbBase, Capabilities->HvPciSwiotlbSize});
        }

        m_swiotlbConfigured = true;
    }

    WSL_LOG(
        "WSLCApplyGuestCapabilities",
        TraceLoggingValue(Capabilities->HvPciSwiotlbBase, "HvPciSwiotlbBase"),
        TraceLoggingValue(Capabilities->HvPciSwiotlbSize, "HvPciSwiotlbSize"));

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::GetTerminationEvent(_Out_ HANDLE* Event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Event);

    *Event = wslutil::DuplicateHandle(m_vmExitEvent.get());

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::MapVirtioNetPort(
    _In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress, _Out_ USHORT* AllocatedHostPort)
try
{
    RETURN_HR_IF(E_POINTER, AllocatedHostPort == nullptr || ListenAddress == nullptr);

    *AllocatedHostPort = 0;

    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_UNEXPECTED, !m_networking.has_value() || !m_networking->MapPort);
    return m_networking->MapPort(CreateListenAddress(ListenAddress, HostPort), GuestPort, Protocol, AllocatedHostPort);
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::UnmapVirtioNetPort(_In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress)
try
{
    RETURN_HR_IF(E_POINTER, ListenAddress == nullptr);

    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_UNEXPECTED, !m_networking.has_value() || !m_networking->UnmapPort);
    return m_networking->UnmapPort(CreateListenAddress(ListenAddress, HostPort), GuestPort, Protocol);
}
CATCH_RETURN()

HRESULT WSLCVirtualMachineHost::GetTerminationReason(_Out_ WSLCVirtualMachineTerminationReason* Reason, _Out_ LPWSTR* Details)
try
{
    RETURN_HR_IF(E_POINTER, Reason == nullptr || Details == nullptr);

    *Reason = WSLCVirtualMachineTerminationReasonUnknown;
    *Details = nullptr;

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vmExitEvent.is_signaled());

    const auto information = m_backend->GetTerminationReason();
    *Reason = information.Reason == VmTerminationReason::Shutdown  ? WSLCVirtualMachineTerminationReasonShutdown
              : information.Reason == VmTerminationReason::Crashed ? WSLCVirtualMachineTerminationReasonCrashed
                                                                   : WSLCVirtualMachineTerminationReasonUnknown;
    *Details = wil::make_cotaskmem_string(information.Details.c_str()).release();

    return S_OK;
}
CATCH_RETURN()

namespace wsl::windows::service::wslc {

WSLCVirtualMachineFactory::WSLCVirtualMachineFactory(_In_ const WSLCSessionSettings* Settings, WSLCVirtualMachineBackendFactory BackendFactory) :
    m_backendFactory(std::move(BackendFactory))
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);
    THROW_HR_IF(E_INVALIDARG, !m_backendFactory);

    m_displayName = Settings->DisplayName ? Settings->DisplayName : L"";
    m_storagePath = Settings->StoragePath ? Settings->StoragePath : L"";

    if (Settings->RootVhdOverride != nullptr)
    {
        m_rootVhdOverride.emplace(Settings->RootVhdOverride);
    }

    if (Settings->RootVhdTypeOverride != nullptr)
    {
        m_rootVhdTypeOverride.emplace(Settings->RootVhdTypeOverride);
    }

    // Keep our own duplicate of the dmesg sink so recreated VMs can reuse it.
    if (Settings->DmesgOutput.Handle.File != nullptr && Settings->DmesgOutput.Handle.File != INVALID_HANDLE_VALUE)
    {
        m_dmesgOutput.reset(wslutil::DuplicateHandle(wslutil::FromCOMInputHandle(Settings->DmesgOutput), GENERIC_WRITE | SYNCHRONIZE));
    }

    m_maximumStorageSizeMb = Settings->MaximumStorageSizeMb;
    m_cpuCount = Settings->CpuCount;
    m_memoryMb = Settings->MemoryMb;
    m_bootTimeoutMs = Settings->BootTimeoutMs;
    m_networkingMode = Settings->NetworkingMode;
    m_featureFlags = Settings->FeatureFlags;
    m_hostLoopback = Settings->HostLoopback ? Settings->HostLoopback : "";
    m_storageFlags = Settings->StorageFlags;
}

WSLCSessionSettings WSLCVirtualMachineFactory::BuildSettings()
{
    WSLCSessionSettings settings{};
    settings.DisplayName = m_displayName.c_str();
    settings.StoragePath = m_storagePath.empty() ? nullptr : m_storagePath.c_str();
    settings.MaximumStorageSizeMb = m_maximumStorageSizeMb;
    settings.CpuCount = m_cpuCount;
    settings.MemoryMb = m_memoryMb;
    settings.BootTimeoutMs = m_bootTimeoutMs;
    settings.NetworkingMode = m_networkingMode;
    settings.FeatureFlags = m_featureFlags;
    settings.HostLoopback = m_hostLoopback.empty() ? nullptr : m_hostLoopback.c_str();
    settings.StorageFlags = m_storageFlags;
    settings.RootVhdOverride = m_rootVhdOverride ? m_rootVhdOverride->c_str() : nullptr;
    settings.RootVhdTypeOverride = m_rootVhdTypeOverride ? m_rootVhdTypeOverride->c_str() : nullptr;

    if (m_dmesgOutput)
    {
        settings.DmesgOutput = wslutil::ToCOMInputHandle(m_dmesgOutput.get());
    }

    return settings;
}

HRESULT WSLCVirtualMachineFactory::CreateVirtualMachine(_Out_ IWSLCVirtualMachine** Vm)
try
{
    RETURN_HR_IF(E_POINTER, Vm == nullptr);
    *Vm = nullptr;

    const auto settings = BuildSettings();
    auto vm = Microsoft::WRL::Make<WSLCVirtualMachineHost>(&settings, m_backendFactory);
    THROW_IF_NULL_ALLOC(vm);

    *Vm = vm.Detach();
    return S_OK;
}
CATCH_RETURN()

} // namespace wsl::windows::service::wslc
