/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachine.cpp

Abstract:

    Implementation of IWSLAVirtualMachine - represents a single HCS-based VM instance.

--*/

#include "HcsVirtualMachine.h"
#include <format>
#include "hcs_schema.h"
#include "VirtioNetworking.h"
#include "NatNetworking.h"
#include "wslsecurity.h"
#include "wslutil.h"
#include "lxinitshared.h"
#include "DnsResolver.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using wsl::windows::service::wsla::HcsVirtualMachine;

constexpr auto MAX_VM_CRASH_FILES = 3;
constexpr auto MAX_CRASH_DUMPS = 10;
constexpr auto SAVED_STATE_FILE_EXTENSION = L".vmrs";
constexpr auto SAVED_STATE_FILE_PREFIX = L"saved-state-";
constexpr auto RECEIVE_TIMEOUT = 30 * 1000;

HcsVirtualMachine::HcsVirtualMachine(_In_ const WSLA_SESSION_SETTINGS* Settings)
{
    THROW_HR_IF(E_POINTER, Settings == nullptr);

    // Store the user token.
    m_userToken = wil::shared_handle{wsl::windows::common::security::GetUserToken(TokenImpersonation).release()};
    m_virtioFsClassId = wsl::windows::common::security::IsTokenElevated(m_userToken.get()) ? VIRTIO_FS_ADMIN_CLASS_ID : VIRTIO_FS_CLASS_ID;
    m_crashDumpFolder = GetCrashDumpFolder();

    std::lock_guard lock(m_lock);

    THROW_IF_FAILED(CoCreateGuid(&m_vmId));
    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_featureFlags = static_cast<WSLAFeatureFlags>(Settings->FeatureFlags);
    m_networkingMode = Settings->NetworkingMode;
    m_bootTimeoutMs = Settings->BootTimeoutMs;

    // Build HCS settings
    hcs::ComputeSystem systemSettings{};
    systemSettings.Owner = L"WSL";
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

    // Configure backing page size, fault cluster shift size, and cold discard hint size to favor density (lower vmmem usage).
    //
    // N.B. Cold discard hint size should be a multiple of the fault cluster shift size.
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    if (windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium)
    {
        vmSettings.ComputeTopology.Memory.BackingPageSize = hcs::MemoryBackingPageSize::Small;
        vmSettings.ComputeTopology.Memory.FaultClusterSizeShift = 4;
        vmSettings.ComputeTopology.Memory.DirectMapFaultClusterSizeShift = 4;
    }

    if (helpers::IsVmemmSuffixSupported() && Settings->DisplayName)
    {
        vmSettings.ComputeTopology.Memory.HostingProcessNameSuffix = Settings->DisplayName;
    }

#ifdef _AMD64_

    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    vmSettings.ComputeTopology.Processor.EnablePerfmonPmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
    vmSettings.ComputeTopology.Processor.EnablePerfmonLbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;

#endif

    // Initialize kernel command line.
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLA_ROOT_INIT_ENV) L"=1 panic=-1";
    kernelCmdLine += std::format(L" nr_cpus={}", Settings->CpuCount);

    // Enable timesync workaround to sync on resume from sleep in modern standby.
    kernelCmdLine += L" hv_utils.timesync_implicit=1";

    // Setup dmesg collector with optional DmesgOutput handle.
    // TODO: move dmesg collector to user session process.
    wil::unique_handle dmesgOutputHandle;
    if (Settings->DmesgOutput != 0)
    {
        dmesgOutputHandle.reset(wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(Settings->DmesgOutput)));
    }

    m_dmesgCollector = DmesgCollector::Create(
        m_vmId, m_vmExitEvent, true, false, L"", FeatureEnabled(WslaFeatureFlagsEarlyBootDmesg), std::move(dmesgOutputHandle));

    if (FeatureEnabled(WslaFeatureFlagsEarlyBootDmesg))
    {
        kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
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
    if (!FeatureEnabled(WslaFeatureFlagsPmemVhds))
    {
        auto attachScsiDisk = [&](PCWSTR path, ULONG& lun) {
            lun = m_nextLun++;
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
            return lun;
        };

        ULONG rootLun, modulesLun;
        attachScsiDisk(rootVhdPath.c_str(), rootLun);
        attachScsiDisk(kernelModulesPath.c_str(), modulesLun);
    }
    else
    {
        hcs::VirtualPMemController pmemController{};
        pmemController.Backing = hcs::VirtualPMemBackingType::Virtual;
        ULONG nextDeviceId = 0;
        auto attachPmemDisk = [&](PCWSTR path) {
            auto deviceId = nextDeviceId++;
            hcs::VirtualPMemDevice vhd{};
            vhd.HostPath = path;
            vhd.ReadOnly = true;
            vhd.ImageFormat = hcs::VirtualPMemImageFormat::Vhd1;
            pmemController.Devices[std::to_string(deviceId)] = std::move(vhd);
        };

        attachPmemDisk(rootVhdPath.c_str());
        attachPmemDisk(kernelModulesPath.c_str());
        vmSettings.Devices.VirtualPMem = std::move(pmemController);
    }

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

    WSL_LOG("CreateWSLAVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    // Create and start compute system
    m_computeSystem = hcs::CreateComputeSystem(m_vmIdString.c_str(), json.c_str());

    if (FeatureEnabled(WslaFeatureFlagsVirtioFs) || m_networkingMode == WSLANetworkingModeVirtioProxy)
    {
        m_guestDeviceManager = std::make_shared<::GuestDeviceManager>(m_vmIdString, m_vmId);
    }

    // Configure termination callback
    if (Settings->TerminationCallback)
    {
        m_terminationCallback = Settings->TerminationCallback;
    }

    hcs::RegisterCallback(m_computeSystem.get(), &HcsVirtualMachine::OnVmExitCallback, this);

    // Create a listening socket for mini_init to connect to once the VM is running.
    m_listenSocket = wsl::windows::common::hvsocket::Listen(m_vmId, LX_INIT_UTILITY_VM_INIT_PORT);

    // Start crash dump listener
    auto crashDumpSocket = wsl::windows::common::hvsocket::Listen(m_vmId, LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);
    THROW_LAST_ERROR_IF(!crashDumpSocket);

    // Start the virtual machine
    hcs::StartComputeSystem(m_computeSystem.get(), json.c_str());

    m_crashDumpThread = std::thread{[this, socket = std::move(crashDumpSocket)]() mutable { CollectCrashDumps(std::move(socket)); }};

    // Add GPU to the VM if requested (HCS modify operation)
    if (FeatureEnabled(WslaFeatureFlagsGPU))
    {
        hcs::ModifySettingRequest<hcs::GpuConfiguration> gpuRequest{};
        gpuRequest.ResourcePath = L"VirtualMachine/ComputeTopology/Gpu";
        gpuRequest.RequestType = hcs::ModifyRequestType::Update;
        gpuRequest.Settings.AssignmentMode = hcs::GpuAssignmentMode::Mirror;
        gpuRequest.Settings.AllowVendorExtension = true;
        if (wsl::windows::common::helpers::IsDisableVgpuSettingsSupported())
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

    WSL_LOG("WSLATerminateVm", TraceLoggingValue(forceTerminate, "forced"));

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

    if (m_crashDumpThread.joinable())
    {
        m_crashDumpThread.join();
    }
}

bool HcsVirtualMachine::FeatureEnabled(WSLAFeatureFlags Value) const
{
    return static_cast<ULONG>(m_featureFlags) & static_cast<ULONG>(Value);
}

HRESULT HcsVirtualMachine::GetId(_Out_ GUID* VmId)
try
{
    *VmId = m_vmId;
    return S_OK;
}
CATCH_RETURN()

HRESULT HcsVirtualMachine::AcceptConnection(_Out_ HANDLE* Socket)
try
{
    auto socket = wsl::windows::common::hvsocket::CancellableAccept(m_listenSocket.get(), m_bootTimeoutMs, m_vmExitEvent.get());
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

    if (m_networkingMode == WSLANetworkingModeNone)
    {
        return S_OK;
    }

    // If DNS tunneling is requested, determine if it is supported by the host OS.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        FeatureEnabled(WslaFeatureFlagsDnsTunneling) && FAILED(wsl::core::networking::DnsResolver::LoadDnsResolverMethods()),
        "DNS tunneling is not supported on this version of Windows");

    // Duplicate the socket handles - COM manages the lifetime of the marshalled handles,
    // so we need our own copies to take ownership.
    wil::unique_socket gnsSocketHandle{reinterpret_cast<SOCKET>(helpers::DuplicateHandle(GnsSocket))};
    if (m_networkingMode == WSLANetworkingModeNAT)
    {
        // TODO: refactor this to avoid using wsl config
        wsl::core::Config config(nullptr);
        if (!wsl::core::NatNetworking::IsHyperVFirewallSupported(config))
        {
            config.FirewallConfig.reset();
        }

        // Enable DNS tunneling if requested
        wil::unique_socket dnsSocketHandle;
        if (FeatureEnabled(WslaFeatureFlagsDnsTunneling))
        {
            THROW_HR_IF(E_INVALIDARG, DnsSocket == nullptr);

            dnsSocketHandle.reset(reinterpret_cast<SOCKET>(helpers::DuplicateHandle(*DnsSocket)));
            config.EnableDnsTunneling = true;
            in_addr address{};
            WI_VERIFY(inet_pton(AF_INET, LX_INIT_DNS_TUNNELING_IP_ADDRESS, &address) == 1);
            config.DnsTunnelingIpAddress = address.S_un.S_addr;
        }
        else
        {
            THROW_HR_IF(E_INVALIDARG, DnsSocket != nullptr);
        }

        m_networkEngine = std::make_unique<wsl::core::NatNetworking>(
            m_computeSystem.get(),
            wsl::core::NatNetworking::CreateNetwork(config),
            wsl::core::GnsChannel(std::move(gnsSocketHandle)),
            config,
            std::move(dnsSocketHandle),
            nullptr);
    }
    else if (m_networkingMode == WSLANetworkingModeVirtioProxy)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, DnsSocket != nullptr, "DNS socket should not be provided with virtio proxy networking mode");

        auto flags = wsl::core::VirtioNetworkingFlags::None;
        WI_SetFlagIf(flags, wsl::core::VirtioNetworkingFlags::DnsTunneling, FeatureEnabled(WslaFeatureFlagsDnsTunneling));

        m_networkEngine = std::make_unique<wsl::core::VirtioNetworking>(
            wsl::core::GnsChannel(std::move(gnsSocketHandle)), flags, nullptr, m_guestDeviceManager, m_userToken);
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

    auto grantDiskAccess = [&]() {
        auto runAsUser = wil::impersonate_token(m_userToken.get());
        hcs::GrantVmAccess(m_vmIdString.c_str(), Path);
        disk.AccessGranted = true;
    };

    if (!ReadOnly)
    {
        grantDiskAccess();
    }

    *Lun = m_nextLun++;

    auto result = wil::ResultFromException([&]() { hcs::AddVhd(m_computeSystem.get(), Path, *Lun, ReadOnly); });

    if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !disk.AccessGranted)
    {
        grantDiskAccess();
        hcs::AddVhd(m_computeSystem.get(), Path, *Lun, ReadOnly);
    }
    else
    {
        THROW_IF_FAILED(result);
    }

    m_attachedDisks.emplace(*Lun, std::move(disk));

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

    THROW_IF_FAILED(CoCreateGuid(ShareId));
    auto shareName = wsl::shared::string::GuidToString<wchar_t>(*ShareId, wsl::shared::string::None);

    std::optional<GUID> deviceInstanceId;
    if (!FeatureEnabled(WslaFeatureFlagsVirtioFs))
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
        deviceInstanceId = m_guestDeviceManager->AddGuestDevice(
            VIRTIO_FS_DEVICE_ID, m_virtioFsClassId, shareName.c_str(), L"", WindowsPath, VIRTIO_FS_FLAGS_TYPE_FILES, m_userToken.get());
    }

    m_shares.emplace(*ShareId, deviceInstanceId);

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
    m_vmExitEvent.SetEvent();

    const auto exitStatus = wsl::shared::FromJson<wsl::windows::common::hcs::SystemExitStatus>(Event->EventData);

    auto reason = WSLAVirtualMachineTerminationReasonUnknown;

    if (exitStatus.ExitType.has_value())
    {
        switch (exitStatus.ExitType.value())
        {
        case hcs::NotificationType::ForcedExit:
        case hcs::NotificationType::GracefulExit:
            reason = WSLAVirtualMachineTerminationReasonShutdown;
            break;
        case hcs::NotificationType::UnexpectedExit:
            reason = WSLAVirtualMachineTerminationReasonCrashed;
            break;
        default:
            reason = WSLAVirtualMachineTerminationReasonUnknown;
            break;
        }
    }

    if (m_terminationCallback)
    {
        LOG_IF_FAILED(m_terminationCallback->OnTermination(reason, Event->EventData));
    }
}

void HcsVirtualMachine::OnCrash(const HCS_EVENT* Event)
{
    if (m_crashLogCaptured && m_vmSavedStateCaptured)
    {
        return;
    }

    const auto crashReport = wsl::shared::FromJson<wsl::windows::common::hcs::CrashReport>(Event->EventData);

    if (crashReport.GuestCrashSaveInfo.has_value() && crashReport.GuestCrashSaveInfo->SaveStateFile.has_value())
    {
        m_vmSavedStateCaptured = true;
        EnforceVmSavedStateFileLimit();
    }

    if (!m_crashLogCaptured && !crashReport.CrashLog.empty())
    {
        WriteCrashLog(crashReport.CrashLog);
    }
}

void HcsVirtualMachine::CollectCrashDumps(wil::unique_socket&& listenSocket) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"CrashDumpCollection");

    while (!m_vmExitEvent.is_signaled())
    {
        try
        {
            auto socket = wsl::windows::common::hvsocket::CancellableAccept(listenSocket.get(), INFINITE, m_vmExitEvent.get());
            if (!socket)
            {
                // VM is exiting.
                break;
            }

            THROW_LAST_ERROR_IF(
                setsockopt(listenSocket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&RECEIVE_TIMEOUT, sizeof(RECEIVE_TIMEOUT)) == SOCKET_ERROR);

            auto channel = wsl::shared::SocketChannel{std::move(socket.value()), "crash_dump", m_vmExitEvent.get()};

            const auto& message = channel.ReceiveMessage<LX_PROCESS_CRASH>();
            const char* process = reinterpret_cast<const char*>(&message.Buffer);

            constexpr auto dumpExtension = ".dmp";
            constexpr auto dumpPrefix = "wsl-crash";

            auto filename = std::format("{}-{}-{}-{}-{}{}", dumpPrefix, message.Timestamp, message.Pid, process, message.Signal, dumpExtension);

            std::replace_if(filename.begin(), filename.end(), [](auto e) { return !std::isalnum(e) && e != '.' && e != '-'; }, '_');

            auto fullPath = m_crashDumpFolder / filename;

            WSL_LOG(
                "WSLALinuxCrash",
                TraceLoggingValue(fullPath.c_str(), "FullPath"),
                TraceLoggingValue(message.Pid, "Pid"),
                TraceLoggingValue(message.Signal, "Signal"),
                TraceLoggingValue(process, "process"));

            auto runAsUser = wil::impersonate_token(m_userToken.get());
            wsl::windows::common::filesystem::EnsureDirectory(m_crashDumpFolder.c_str());

            // Only delete files that:
            // - have the temporary flag set
            // - start with 'wsl-crash'
            // - end in .dmp
            //
            // This logic is here to prevent accidental user file deletion
            auto pred = [&dumpExtension, &dumpPrefix](const auto& e) {
                return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
                       e.path().extension() == dumpExtension && e.path().has_filename() &&
                       e.path().filename().string().find(dumpPrefix) == 0;
            };

            wsl::windows::common::wslutil::EnforceFileLimit(m_crashDumpFolder.c_str(), MAX_CRASH_DUMPS, pred);

            wil::unique_hfile file{CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
            THROW_LAST_ERROR_IF(!file);

            channel.SendResultMessage<std::int32_t>(0);
            wsl::windows::common::relay::InterruptableRelay(reinterpret_cast<HANDLE>(channel.Socket()), file.get(), nullptr);
        }
        CATCH_LOG()
    }
}

std::filesystem::path HcsVirtualMachine::GetCrashDumpFolder()
{
    auto tempPath = wsl::windows::common::filesystem::GetTempFolderPath(m_userToken.get());
    return tempPath / L"wsla-crashes";
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
    m_crashLogCaptured = true;
}
