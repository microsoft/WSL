/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachine.cpp

Abstract:

    Implementation of IWSLCVirtualMachine - represents a single HCS-based VM instance.

--*/

#include "HcsVirtualMachine.h"
#include <format>
#include <string>
#include <string_view>
#include "hcs_schema.h"
#include "ConsommeNetworking.h"
#include "NatNetworking.h"
#include "wslsecurity.h"
#include "wslutil.h"
#include "lxinitshared.h"
#include "DnsResolver.h"
#include "string.hpp"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using wsl::windows::service::wslc::HcsVirtualMachine;

constexpr auto MAX_VM_CRASH_FILES = 3;
constexpr auto SAVED_STATE_FILE_EXTENSION = L".vmrs";
constexpr auto SAVED_STATE_FILE_PREFIX = L"saved-state-";

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

// Replace any character outside the conservative ASCII allowlist with '_' so the
// result is safe to use as the HCS HostingProcessNameSuffix (which becomes the
// vmmem-XXX process name visible in Task Manager and parsed by various tooling).
std::wstring SanitizeHostingProcessNameSuffix(std::wstring_view name)
{
    constexpr std::wstring_view c_allowed = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
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

HcsVirtualMachine::HcsVirtualMachine(_In_ const WSLCSessionSettings* Settings)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);

    // Store the user token.
    m_userToken = wil::shared_handle{wsl::windows::common::security::GetUserToken(TokenImpersonation).release()};
    m_virtioFsClassId = wsl::windows::common::security::IsTokenElevated(m_userToken.get()) ? VIRTIO_FS_ADMIN_CLASS_ID : VIRTIO_FS_CLASS_ID;
    m_crashDumpFolder = GetCrashDumpFolder();

    std::lock_guard lock(m_lock);

    THROW_IF_FAILED(CoCreateGuid(&m_vmId));
    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_featureFlags = Settings->FeatureFlags;
    m_networkingMode = Settings->NetworkingMode;
    m_bootTimeoutMs = Settings->BootTimeoutMs;

    // Build HCS settings
    hcs::ComputeSystem systemSettings{};
    systemSettings.Owner = Settings->DisplayName ? Settings->DisplayName : L"WSLC";
    systemSettings.ShouldTerminateOnLastHandleClosed = true;

    // Determine which schema version to use based on the Windows version. Windows 10 does not support
    // newer schema versions and some features may be disabled as a result.
    if (wsl::windows::common::helpers::IsWindows11OrAbove())
    {
        systemSettings.SchemaVersion.Major = 2;
        systemSettings.SchemaVersion.Minor = 7;
    }
    else
    {
        systemSettings.SchemaVersion.Major = 2;
        systemSettings.SchemaVersion.Minor = 3;
    }

    hcs::VirtualMachine vmSettings{};
    vmSettings.StopOnReset = true;
    vmSettings.Chipset.UseUtc = true;

    // Ensure the 2MB granularity enforced by HCS.
    vmSettings.ComputeTopology.Memory.SizeInMB = Settings->MemoryMb & ~0x1;
    vmSettings.ComputeTopology.Memory.AllowOvercommit = true;
    vmSettings.ComputeTopology.Memory.EnableDeferredCommit = true;
    vmSettings.ComputeTopology.Memory.EnableColdDiscardHint = true;
    vmSettings.ComputeTopology.Processor.Count = Settings->CpuCount;

    // Configure backing page size, fault cluster shift size, and page reporting order to favor density (lower vmmem usage).
    //
    // N.B. Page reporting order must be >= fault cluster size shift.
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    int pageReportingOrder;
    if (windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium)
    {
        vmSettings.ComputeTopology.Memory.BackingPageSize = hcs::MemoryBackingPageSize::Small;
        vmSettings.ComputeTopology.Memory.FaultClusterSizeShift = 4;
        vmSettings.ComputeTopology.Memory.DirectMapFaultClusterSizeShift = 4;
        pageReportingOrder = 5; // 128k
    }
    else
    {
        pageReportingOrder = 9; // 2MB
    }

    if (helpers::IsVmemmSuffixSupported() && Settings->DisplayName)
    {
        // The vmmem-XXX process name shown in Task Manager (and parsed by tooling)
        // can't tolerate spaces / unicode / etc., so sanitize before use. Note that
        // Settings->DisplayName itself (e.g. the HCS Owner) is left untouched.
        vmSettings.ComputeTopology.Memory.HostingProcessNameSuffix = SanitizeHostingProcessNameSuffix(Settings->DisplayName);
    }

#ifdef _AMD64_

    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    vmSettings.ComputeTopology.Processor.EnablePerfmonPmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
    vmSettings.ComputeTopology.Processor.EnablePerfmonLbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;

#endif

    // Compute a swiotlb device-options token sized to fit this VM's RAM, used by the kernel
    // command line, virtiofs shares, and the Consomme virtio-net adapter.
    // Only needed when a virtio device that requires bounce buffers will be attached.
    ULONG64 swiotlbSizeBytes = 0;
    if (FeatureEnabled(WslcFeatureFlagsVirtioFs) || m_networkingMode == WSLCNetworkingModeConsomme)
    {
        swiotlbSizeBytes = helpers::ComputeDefaultSwiotlbConfig(static_cast<UINT64>(Settings->MemoryMb) * _1MB);
    }

    // Initialize kernel command line.
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLC_ROOT_INIT_ENV) L"=1 panic=-1";
    kernelCmdLine += std::format(L" nr_cpus={}", Settings->CpuCount);
    helpers::AppendCommonKernelCommandLine(kernelCmdLine, pageReportingOrder, swiotlbSizeBytes);

    // Setup dmesg collector with optional DmesgOutput handle.
    // TODO: move dmesg collector to user session process.
    // N.B. 'DmesgOutput' needs to be duplicated since COM will close it when this call completes.
    wil::unique_handle dmesgOutputHandle;
    if (Settings->DmesgOutput.Handle.File != nullptr && Settings->DmesgOutput.Handle.File != INVALID_HANDLE_VALUE)
    {
        dmesgOutputHandle.reset(wslutil::DuplicateHandle(wslutil::FromCOMInputHandle(Settings->DmesgOutput), GENERIC_WRITE | SYNCHRONIZE));
    }

    m_dmesgCollector = DmesgCollector::Create(
        m_vmId, m_vmExitEvent.get(), true, false, L"", FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg), std::move(dmesgOutputHandle));

    if (FeatureEnabled(WslcFeatureFlagsEarlyBootDmesg))
    {
        if constexpr (!wsl::shared::Arm64)
        {
            kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
        }
        else
        {
            kernelCmdLine += L" earlycon=pl011,0xeffec000,115200";
        }

        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
    }

    if (helpers::IsVirtioSerialConsoleSupported())
    {
        kernelCmdLine += L" console=hvc0 debug";
        vmSettings.Devices.VirtioSerial.emplace();
        hcs::VirtioSerialPort virtioPort{};
        virtioPort.Name = L"hvc0";
        virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);
    }

    // Set up boot params.
    //
    // N.B. Linux kernel direct boot is not yet supported on ARM64.
    auto basePath = wslutil::GetBasePath();

#ifdef WSL_KERNEL_PATH
    auto kernelPath = std::filesystem::path(WSL_KERNEL_PATH);
#else
    auto kernelPath = std::filesystem::path(basePath) / L"tools" / LXSS_VM_MODE_KERNEL_NAME;
#endif

    if constexpr (!wsl::shared::Arm64)
    {
        vmSettings.Chipset.LinuxKernelDirect.emplace();
        vmSettings.Chipset.LinuxKernelDirect->KernelFilePath = kernelPath.wstring();
        vmSettings.Chipset.LinuxKernelDirect->InitRdPath = (basePath / L"tools" / LXSS_VM_MODE_INITRD_NAME).c_str();
        vmSettings.Chipset.LinuxKernelDirect->KernelCmdLine = kernelCmdLine;
    }
    else
    {
        auto bootThis = hcs::UefiBootEntry{};
        bootThis.DeviceType = hcs::UefiBootDevice::VmbFs;
        bootThis.VmbFsRootPath = (basePath / L"tools").c_str();
        bootThis.DevicePath = L"\\" LXSS_VM_MODE_KERNEL_NAME;
        bootThis.OptionalData = kernelCmdLine;
        hcs::Uefi uefiSettings{};
        uefiSettings.BootThis = std::move(bootThis);
        vmSettings.Chipset.Uefi = std::move(uefiSettings);
    }

#ifdef WSL_KERNEL_MODULES_PATH
    auto kernelModulesPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));
#else
    auto kernelModulesPath = basePath / L"tools" / L"modules.vhd";
#endif

    // Get root VHD path
    std::filesystem::path rootVhdPath;
    if (Settings->RootVhdOverride != nullptr)
    {
        rootVhdPath = Settings->RootVhdOverride;
    }
    else
    {
#ifdef WSL_SYSTEM_DISTRO_PATH
        rootVhdPath = TEXT(WSL_SYSTEM_DISTRO_PATH);
#else
        rootVhdPath = std::filesystem::path(wslutil::GetMsiPackagePath().value()) / L"system.vhd";
#endif
    }

    // Setup boot VHDs
    hcs::Scsi scsiController{};
    auto attachScsiDisk = [&](PCWSTR path) {
        const ULONG lun = AllocateLun();
        hcs::Attachment disk{};
        disk.Type = hcs::AttachmentType::VirtualDisk;
        disk.Path = path;
        disk.ReadOnly = true;
        disk.SupportCompressedVolumes = true;
        disk.AlwaysAllowSparseFiles = true;
        disk.SupportEncryptedFiles = true;
        scsiController.Attachments[std::to_string(lun)] = std::move(disk);
        DiskInfo diskInfo{path};
        m_attachedDisks.emplace(lun, std::move(diskInfo));
    };

    attachScsiDisk(rootVhdPath.c_str());
    attachScsiDisk(kernelModulesPath.c_str());

    vmSettings.Devices.Scsi["0"] = std::move(scsiController);

    // Setup HvSocket security
    auto tokenUser = wil::get_token_information<TOKEN_USER>(m_userToken.get());
    wil::unique_hlocal_string userSidString;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(tokenUser->User.Sid, &userSidString));

    std::wstring securityDescriptor = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", userSidString.get());
    hcs::HvSocket hvSocketConfig{};
    hvSocketConfig.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocketConfig.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    vmSettings.Devices.HvSocket = std::move(hvSocketConfig);

    // Enable .vmrs dump collection if supported.
    if (wsl::windows::common::helpers::IsWindows11OrAbove())
    {
        CreateVmSavedStateFile(m_userToken.get());
        if (!m_vmSavedStateFile.empty())
        {
            hcs::DebugOptions debugOptions{};
            debugOptions.BugcheckSavedStateFileName = m_vmSavedStateFile;
            vmSettings.DebugOptions = std::move(debugOptions);
        }
    }

    systemSettings.VirtualMachine = std::move(vmSettings);
    auto json = wsl::shared::ToJsonW(systemSettings);

    WSL_LOG("CreateWSLCVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    // Create and start compute system
    m_computeSystem = hcs::CreateComputeSystem(m_vmIdString.c_str(), json.c_str());

    if (FeatureEnabled(WslcFeatureFlagsVirtioFs) || m_networkingMode == WSLCNetworkingModeConsomme)
    {
        m_guestDeviceManager = std::make_shared<::GuestDeviceManager>(m_vmIdString, m_vmId);
    }

    hcs::RegisterCallback(m_computeSystem.get(), &HcsVirtualMachine::OnVmExitCallback, this);

    // Create a listening socket for mini_init to connect to once the VM is running.
    m_listenSocket = wsl::windows::common::hvsocket::Listen(m_vmId, LX_INIT_UTILITY_VM_INIT_PORT);

    // Start the virtual machine
    hcs::StartComputeSystem(m_computeSystem.get(), json.c_str());

    // Add GPU to the VM if requested
    if (FeatureEnabled(WslcFeatureFlagsGPU))
    {
        hcs::ModifySettingRequest<hcs::GpuConfiguration> gpuRequest{};
        gpuRequest.ResourcePath = L"VirtualMachine/ComputeTopology/Gpu";
        gpuRequest.RequestType = hcs::ModifyRequestType::Update;
        gpuRequest.Settings.AssignmentMode = hcs::GpuAssignmentMode::Mirror;
        gpuRequest.Settings.AllowVendorExtension = true;
        if (wsl::windows::common::hcs::IsDisableVgpuSettingsSupported())
        {
            gpuRequest.Settings.DisableGdiAcceleration = true;
            gpuRequest.Settings.DisablePresentation = true;
        }

        hcs::ModifyComputeSystem(m_computeSystem.get(), wsl::shared::ToJsonW(gpuRequest).c_str());
    }
}

HcsVirtualMachine::~HcsVirtualMachine()
{
    std::lock_guard lock(m_lock);

    // Wait up to 5 seconds for the VM to terminate gracefully.
    bool forceTerminate = false;
    if (!m_vmExitEvent.wait(5000))
    {
        forceTerminate = true;
        try
        {
            hcs::TerminateComputeSystem(m_computeSystem.get());
        }
        CATCH_LOG()
    }

    WSL_LOG("WSLCTerminateVm", TraceLoggingValue(forceTerminate, "forced"));

    // N.B. Destruction order matters: the networking engine and device manager must be torn down
    // before the compute system handle is closed. The networking engine holds a shared_ptr to
    // GuestDeviceManager, so it must be released first for the device manager reset to be effective.
    m_networkEngine.reset();
    m_guestDeviceManager.reset();
    m_computeSystem.reset();

    // Revoke VM access for attached disks
    for (const auto& e : m_attachedDisks)
    {
        try
        {
            if (e.second.AccessGranted)
            {
                hcs::RevokeVmAccess(m_vmIdString.c_str(), e.second.Path.c_str());
            }
        }
        CATCH_LOG()
    }

    // If the VM did not crash, the saved state file should be empty, so we can remove it.
    if (!m_vmSavedStateFile.empty() && !m_vmSavedStateCaptured)
    {
        try
        {
            WI_ASSERT(std::filesystem::is_empty(m_vmSavedStateFile));
            std::filesystem::remove(m_vmSavedStateFile);
        }
        CATCH_LOG()
    }
}

bool HcsVirtualMachine::FeatureEnabled(WSLCFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

HRESULT HcsVirtualMachine::GetId(_Out_ GUID* VmId)
try
{
    RETURN_HR_IF_NULL(E_POINTER, VmId);

    *VmId = m_vmId;
    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::AcceptConnection(_Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Socket);

    auto socket = socket::CancellableAccept(m_listenSocket.get(), m_bootTimeoutMs, m_vmExitEvent.get());
    THROW_HR_IF(E_ABORT, !socket.has_value());

    *Socket = reinterpret_cast<HANDLE>(socket->release());
    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::ConfigureNetworking(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket)
try
{
    std::lock_guard lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_networkEngine != nullptr);

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

    if (m_networkingMode == WSLCNetworkingModeNAT)
    {
        // TODO: refactor this to avoid using wsl config
        m_natConfig.emplace(nullptr);
        if (!wsl::core::NatNetworking::IsHyperVFirewallSupported(*m_natConfig))
        {
            m_natConfig->FirewallConfig.reset();
        }

        // Enable DNS tunneling if a DNS socket was provided
        if (FeatureEnabled(WslcFeatureFlagsDnsTunneling))
        {
            WI_ASSERT(dnsSocketHandle);

            m_natConfig->EnableDnsTunneling = true;
            in_addr address{};
            WI_VERIFY(inet_pton(AF_INET, LX_INIT_DNS_TUNNELING_IP_ADDRESS, &address) == 1);
            m_natConfig->DnsTunnelingIpAddress = address.S_un.S_addr;
        }

        m_networkEngine = std::make_unique<wsl::core::NatNetworking>(
            m_computeSystem.get(),
            wsl::core::NatNetworking::CreateNetwork(*m_natConfig),
            wsl::core::GnsChannel(std::move(gnsSocketHandle)),
            *m_natConfig,
            std::move(dnsSocketHandle),
            nullptr);
    }
    else if (m_networkingMode == WSLCNetworkingModeConsomme)
    {
        wsl::core::ConsommeNetworkingFlags flags = wsl::core::ConsommeNetworkingFlags::Ipv6;
        if (FeatureEnabled(WslcFeatureFlagsDnsTunneling))
        {
            WI_SetFlag(flags, wsl::core::ConsommeNetworkingFlags::DnsTunneling);
        }

        if (!FeatureEnabled(WslcFeatureFlagsPortRelayWslRelay))
        {
            WI_SetFlag(flags, wsl::core::ConsommeNetworkingFlags::LocalhostRelay);
        }

        m_networkEngine = std::make_unique<wsl::core::ConsommeNetworking>(
            wsl::core::GnsChannel(std::move(gnsSocketHandle)), flags, nullptr, m_guestDeviceManager, m_userToken, m_swiotlbOption);
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %lu", m_networkingMode);
    }

    m_networkEngine->Initialize();

    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::AttachDisk(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun)
try
{
    RETURN_HR_IF(E_POINTER, Path == nullptr || Lun == nullptr);

    std::lock_guard lock(m_lock);

    DiskInfo disk{Path};
    const ULONG allocatedLun = AllocateLun();

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        if (disk.AccessGranted)
        {
            hcs::RevokeVmAccess(m_vmIdString.c_str(), disk.Path.c_str());
        }

        FreeLun(allocatedLun);
    });

    auto grantDiskAccess = [&]() {
        auto runAsUser = wil::impersonate_token(m_userToken.get());
        hcs::GrantVmAccess(m_vmIdString.c_str(), Path);
        disk.AccessGranted = true;
    };

    if (!ReadOnly)
    {
        grantDiskAccess();
    }

    auto result = wil::ResultFromException([&]() { hcs::AddVhd(m_computeSystem.get(), Path, allocatedLun, ReadOnly); });

    if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !disk.AccessGranted)
    {
        grantDiskAccess();
        hcs::AddVhd(m_computeSystem.get(), Path, allocatedLun, ReadOnly);
    }
    else
    {
        THROW_IF_FAILED(result);
    }

    m_attachedDisks.emplace(allocatedLun, std::move(disk));

    cleanup.release();

    *Lun = allocatedLun;
    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock(m_lock);

    auto it = m_attachedDisks.find(Lun);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    hcs::RemoveScsiDisk(m_computeSystem.get(), Lun);

    FreeLun(Lun);

    if (it->second.AccessGranted)
    {
        hcs::RevokeVmAccess(m_vmIdString.c_str(), it->second.Path.c_str());
    }

    m_attachedDisks.erase(it);

    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::AddShare(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId)
try
{
    RETURN_HR_IF(E_POINTER, WindowsPath == nullptr || ShareId == nullptr);

    std::lock_guard lock(m_lock);

    GUID shareIdLocal;
    THROW_IF_FAILED(CoCreateGuid(&shareIdLocal));
    auto shareName = wsl::shared::string::GuidToString<wchar_t>(shareIdLocal, wsl::shared::string::None);

    // Add the share entry upfront so the emplace cannot fail after the device is created.
    auto it = m_shares.emplace(shareIdLocal, std::nullopt).first;
    auto cleanup = wil::scope_exit([&]() { m_shares.erase(it); });

    if (!FeatureEnabled(WslcFeatureFlagsVirtioFs))
    {
        auto flags = hcs::Plan9ShareFlags::AllowOptions;
        WI_SetFlagIf(flags, hcs::Plan9ShareFlags::ReadOnly, ReadOnly);
        hcs::AddPlan9Share(
            m_computeSystem.get(),
            shareName.c_str(),
            shareName.c_str(),
            WindowsPath,
            LX_INIT_UTILITY_VM_PLAN9_PORT,
            flags,
            m_userToken.get());
    }
    else
    {
        std::wstring options = ReadOnly ? L"ro" : L"";
        auto appendOption = [&options](const std::wstring& option) {
            if (option.empty())
            {
                return;
            }

            if (!options.empty())
            {
                options += L";";
            }

            options += option;
        };

        appendOption(m_swiotlbOption);
        appendOption(c_vcpusOption);

        it->second = m_guestDeviceManager->AddGuestDevice(
            VIRTIO_FS_DEVICE_ID,
            m_virtioFsClassId,
            shareName.c_str(),
            options.c_str(),
            WindowsPath,
            VIRTIO_FS_FLAGS_TYPE_FILES,
            m_userToken.get());
    }

    cleanup.release();

    *ShareId = shareIdLocal;
    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::RemoveShare(_In_ REFGUID ShareId)
try
{
    std::lock_guard lock(m_lock);

    auto it = m_shares.find(ShareId);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_shares.end());

    if (!it->second.has_value())
    {
        auto shareName = wsl::shared::string::GuidToString<wchar_t>(it->first, wsl::shared::string::None);
        hcs::RemovePlan9Share(m_computeSystem.get(), shareName.c_str(), LX_INIT_UTILITY_VM_PLAN9_PORT);
    }
    else
    {
        m_guestDeviceManager->RemoveGuestDevice(VIRTIO_FS_DEVICE_ID, it->second.value());
    }

    m_shares.erase(it);

    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::ApplyGuestCapabilities(_In_ const WSLCGuestCapabilities* Capabilities)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Capabilities);

    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_INVALIDARG, !m_swiotlbOption.empty());

    if (Capabilities->HvPciSwiotlbBase != 0 && Capabilities->HvPciSwiotlbSize != 0)
    {
        m_swiotlbOption = std::format(L"swiotlb=0x{:x},{}", Capabilities->HvPciSwiotlbBase, Capabilities->HvPciSwiotlbSize);
    }

    WSL_LOG(
        "WSLCApplyGuestCapabilities",
        TraceLoggingValue(Capabilities->HvPciSwiotlbBase, "HvPciSwiotlbBase"),
        TraceLoggingValue(Capabilities->HvPciSwiotlbSize, "HvPciSwiotlbSize"));

    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::GetTerminationEvent(_Out_ HANDLE* Event)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Event);

    *Event = wslutil::DuplicateHandle(m_vmExitEvent.get());

    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::MapVirtioNetPort(_In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress, _Out_ USHORT* AllocatedHostPort)
try
{
    RETURN_HR_IF(E_POINTER, AllocatedHostPort == nullptr || ListenAddress == nullptr);

    *AllocatedHostPort = 0;

    std::lock_guard lock(m_lock);

    auto* consommeNet = dynamic_cast<wsl::core::ConsommeNetworking*>(m_networkEngine.get());
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), consommeNet == nullptr);

    return consommeNet->MapPort(CreateListenAddress(ListenAddress, HostPort), GuestPort, Protocol, AllocatedHostPort);
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::UnmapVirtioNetPort(_In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress)
try
{
    RETURN_HR_IF(E_POINTER, ListenAddress == nullptr);

    std::lock_guard lock(m_lock);

    auto* consommeNet = dynamic_cast<wsl::core::ConsommeNetworking*>(m_networkEngine.get());
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), consommeNet == nullptr);

    return consommeNet->UnmapPort(CreateListenAddress(ListenAddress, HostPort), GuestPort, Protocol);
}
CATCH_RETURN()

void CALLBACK HcsVirtualMachine::OnVmExitCallback(HCS_EVENT* Event, void* Context)
try
{
    WSL_LOG(
        "OnVmExitCallback",
        TraceLoggingValue(Event->EventData, "details"),
        TraceLoggingValue(static_cast<int>(Event->Type), "type"));

    auto* vm = reinterpret_cast<HcsVirtualMachine*>(Context);
    if (Event->Type == HcsEventSystemExited)
    {
        vm->OnExit(Event);
    }
    else if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        vm->OnCrash(Event);
    }
}
CATCH_LOG()

void HcsVirtualMachine::OnExit(const HCS_EVENT* Event)
{
    const auto exitStatus = wsl::shared::FromJson<wsl::windows::common::hcs::SystemExitStatus>(Event->EventData);

    auto reason = WSLCVirtualMachineTerminationReasonUnknown;

    if (exitStatus.ExitType.has_value())
    {
        switch (exitStatus.ExitType.value())
        {
        case hcs::NotificationType::ForcedExit:
        case hcs::NotificationType::GracefulExit:
            reason = WSLCVirtualMachineTerminationReasonShutdown;
            break;
        case hcs::NotificationType::UnexpectedExit:
            reason = WSLCVirtualMachineTerminationReasonCrashed;
            break;
        default:
            reason = WSLCVirtualMachineTerminationReasonUnknown;
            break;
        }
    }

    // Cache the termination reason and details before signaling the exit event. These fields are
    // written once here (OnExit fires once and m_vmExitEvent is never reset) and published to readers
    // by the SetEvent below; GetTerminationReason only reads them after observing the signaled event.
    m_terminationReason = reason;
    m_terminationDetails = Event->EventData;

    m_vmExitEvent.SetEvent();
}

HRESULT HcsVirtualMachine::GetTerminationReason(_Out_ WSLCVirtualMachineTerminationReason* Reason, _Out_ LPWSTR* Details)
try
{
    RETURN_HR_IF(E_POINTER, Reason == nullptr || Details == nullptr);

    *Reason = WSLCVirtualMachineTerminationReasonUnknown;
    *Details = nullptr;

    // m_terminationReason/m_terminationDetails are written once in OnExit before m_vmExitEvent is
    // signaled and never modified afterward, so observing the signaled event safely publishes them.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_vmExitEvent.is_signaled());

    *Reason = m_terminationReason;
    *Details = wil::make_cotaskmem_string(m_terminationDetails.c_str()).release();

    return S_OK;
}
CATCH_RETURN()

void HcsVirtualMachine::OnCrash(const HCS_EVENT* Event)
{
    if (m_crashLogCaptured.load() && m_vmSavedStateCaptured.load())
    {
        return;
    }

    const auto crashReport = wsl::shared::FromJson<wsl::windows::common::hcs::CrashReport>(Event->EventData);

    if (crashReport.GuestCrashSaveInfo.has_value() && crashReport.GuestCrashSaveInfo->SaveStateFile.has_value())
    {
        if (!m_vmSavedStateCaptured.exchange(true))
        {
            auto resetFlag = wil::scope_exit([&]() noexcept { m_vmSavedStateCaptured.store(false); });
            EnforceVmSavedStateFileLimit();
            resetFlag.release();
        }
    }

    if (!crashReport.CrashLog.empty())
    {
        if (!m_crashLogCaptured.exchange(true))
        {
            auto resetFlag = wil::scope_exit([&]() noexcept { m_crashLogCaptured.store(false); });
            WriteCrashLog(crashReport.CrashLog);
            resetFlag.release();
        }
    }
}

std::filesystem::path HcsVirtualMachine::GetCrashDumpFolder()
{
    auto tempPath = wsl::windows::common::filesystem::GetTempFolderPath(m_userToken.get());
    return tempPath / L"wslc-crashes";
}

void HcsVirtualMachine::CreateVmSavedStateFile(HANDLE InUserToken)
{
    auto runAsUser = wil::impersonate_token(InUserToken);

    const auto filename = std::format(L"saved-state-{}-{}.vmrs", std::time(nullptr), m_vmIdString);
    auto savedStateFile = m_crashDumpFolder / filename;

    wsl::windows::common::filesystem::EnsureDirectory(m_crashDumpFolder.c_str());

    wil::unique_handle file{CreateFileW(savedStateFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    THROW_LAST_ERROR_IF(!file);

    hcs::GrantVmAccess(m_vmIdString.c_str(), savedStateFile.c_str());
    m_vmSavedStateFile = savedStateFile;
}

void HcsVirtualMachine::EnforceVmSavedStateFileLimit()
{
    auto runAsUser = wil::impersonate_token(m_userToken.get());

    auto pred = [](const auto& e) {
        return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
               e.path().extension() == SAVED_STATE_FILE_EXTENSION && e.path().has_filename() &&
               e.path().filename().wstring().find(SAVED_STATE_FILE_PREFIX) == 0 && e.file_size() > 0;
    };

    wsl::windows::common::wslutil::EnforceFileLimit(m_crashDumpFolder.c_str(), MAX_VM_CRASH_FILES + 1, pred);
}

void HcsVirtualMachine::WriteCrashLog(const std::wstring& crashLog)
{
    auto runAsUser = wil::impersonate_token(m_userToken.get());

    constexpr auto c_extension = L".txt";
    constexpr auto c_prefix = L"kernel-panic-";
    const auto filename = std::format(L"{}{}-{}{}", c_prefix, std::time(nullptr), m_vmIdString, c_extension);
    auto filePath = m_crashDumpFolder / filename;

    WI_ASSERT(std::filesystem::exists(m_crashDumpFolder));
    WI_ASSERT(std::filesystem::is_directory(m_crashDumpFolder));

    auto pred = [&c_extension, &c_prefix](const auto& e) {
        return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
               e.path().extension() == c_extension && e.path().has_filename() && e.path().filename().wstring().find(c_prefix) == 0;
    };

    wsl::windows::common::wslutil::EnforceFileLimit(m_crashDumpFolder.c_str(), MAX_VM_CRASH_FILES, pred);

    {
        std::wofstream outputFile(filePath.wstring());
        THROW_HR_IF(E_UNEXPECTED, !outputFile.is_open());

        outputFile << crashLog;
        THROW_HR_IF(E_UNEXPECTED, outputFile.fail());
    }

    THROW_IF_WIN32_BOOL_FALSE(SetFileAttributesW(filePath.c_str(), FILE_ATTRIBUTE_TEMPORARY));
}

ULONG HcsVirtualMachine::AllocateLun()
{
    for (ULONG index = 0; index < gsl::narrow_cast<ULONG>(m_lunBitmap.size()); index += 1)
    {
        if (!m_lunBitmap[index])
        {
            m_lunBitmap[index] = true;
            return index;
        }
    }

    THROW_HR(WSL_E_TOO_MANY_DISKS_ATTACHED);
}

void HcsVirtualMachine::FreeLun(ULONG Lun)
{
    THROW_HR_IF(E_BOUNDS, Lun >= m_lunBitmap.size());
    THROW_HR_IF(E_INVALIDARG, !m_lunBitmap[Lun]);

    m_lunBitmap[Lun] = false;
}

namespace wsl::windows::service::wslc {

WSLCVirtualMachineFactory::WSLCVirtualMachineFactory(_In_ const WSLCSessionSettings* Settings)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);

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
    auto vm = Microsoft::WRL::Make<HcsVirtualMachine>(&settings);
    THROW_IF_NULL_ALLOC(vm);

    *Vm = vm.Detach();
    return S_OK;
}
CATCH_RETURN()

} // namespace wsl::windows::service::wslc
