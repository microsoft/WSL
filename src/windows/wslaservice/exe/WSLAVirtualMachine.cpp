/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVirtualMachine.cpp

Abstract:

    Class for the WSLA virtual machine.

--*/

#include "WSLAVirtualMachine.h"
#include <format>
#include <filesystem>
#include "hcs_schema.h"
#include "VirtioNetworking.h"
#include "NatNetworking.h"
#include "ServiceProcessLauncher.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using helpers::WindowsVersion;
using wsl::windows::service::wsla::WSLAProcess;
using wsl::windows::service::wsla::WSLAVirtualMachine;

constexpr auto MAX_VM_CRASH_FILES = 3;
constexpr auto MAX_CRASH_DUMPS = 10;
constexpr auto SAVED_STATE_FILE_EXTENSION = L".vmrs";
constexpr auto SAVED_STATE_FILE_PREFIX = L"saved-state-";
constexpr auto RECEIVE_TIMEOUT = 30 * 1000;
constexpr auto CONTAINER_PORT_RANGE = std::pair<uint16_t, uint16_t>(20002, 65535);

static_assert(c_ephemeralPortRange.second < CONTAINER_PORT_RANGE.first);

WSLAVirtualMachine::WSLAVirtualMachine(WSLAVirtualMachine::Settings&& Settings, PSID UserSid) :
    m_settings(std::move(Settings)), m_userSid(UserSid)
{
    THROW_IF_FAILED(CoCreateGuid(&m_vmId));

    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    m_virtioFsClassId = wsl::windows::common::security::IsTokenElevated(m_userToken.get()) ? VIRTIO_FS_ADMIN_CLASS_ID : VIRTIO_FS_CLASS_ID;
    m_crashDumpFolder = GetCrashDumpFolder();
}

void WSLAVirtualMachine::OnSessionTerminated()
{
    // This method is called when the WSLA session is terminated.
    // When that happens, signal the terminating event to cancel any pending operation

    std::lock_guard mutex(m_lock);
    if (m_vmTerminatingEvent.is_signaled())
    {
        return;
    }

    WSL_LOG("WSLASignalTerminating", TraceLoggingValue(m_running, "running"));

    m_vmTerminatingEvent.SetEvent();
}

WSLAVirtualMachine::~WSLAVirtualMachine()
{
    WSL_LOG("WSLATerminateVmStart", TraceLoggingValue(m_running, "running"));
    if (!m_computeSystem)
    {
        // If m_computeSystem is null, don't try to stop the VM since it never started.
        return;
    }

    m_initChannel.Close();

    bool forceTerminate = false;

    // Wait up to 5 seconds for the VM to terminate.
    if (!m_vmExitEvent.wait(5000))
    {
        forceTerminate = true;
        try
        {
            wsl::windows::common::hcs::TerminateComputeSystem(m_computeSystem.get());
        }
        CATCH_LOG()
    }

    WSL_LOG("WSLATerminateVm", TraceLoggingValue(forceTerminate, "forced"), TraceLoggingValue(m_running, "running"));

    // Shutdown DeviceHostProxy before resetting compute system
    m_guestDeviceManager.reset();

    m_computeSystem.reset();

    for (const auto& e : m_attachedDisks)
    {
        try
        {
            if (e.second.AccessGranted)
            {
                wsl::windows::common::hcs::RevokeVmAccess(m_vmIdString.c_str(), e.second.Path.c_str());
            }
        }
        CATCH_LOG()
    }

    if (!m_vmSavedStateFile.empty() && !m_vmSavedStateCaptured)
    {
        // If the VM did not crash, the saved state file should be empty, so we can remove it.
        try
        {
            WI_ASSERT(std::filesystem::is_empty(m_vmSavedStateFile));
            std::filesystem::remove(m_vmSavedStateFile);
        }
        CATCH_LOG()
    }

    if (m_processExitThread.joinable())
    {
        m_processExitThread.join();
    }

    if (m_crashDumpCollectionThread.joinable())
    {
        m_crashDumpCollectionThread.join();
    }

    // Clear the state of all remaining processes now that the VM has exited.
    // The WSLAProcess object reference will be released when the last COM reference is closed.
    for (auto& e : m_trackedProcesses)
    {
        e->OnVmTerminated();
    }
}

void WSLAVirtualMachine::Start()
{
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
    vmSettings.ComputeTopology.Memory.SizeInMB = m_settings.MemoryMb & ~0x1;
    vmSettings.ComputeTopology.Memory.AllowOvercommit = true;
    vmSettings.ComputeTopology.Memory.EnableDeferredCommit = true;
    vmSettings.ComputeTopology.Memory.EnableColdDiscardHint = true;

    // Configure backing page size, fault cluster shift size, and cold discard hint size to favor density (lower vmmem usage).
    //
    // N.B. Cold discard hint size should be a multiple of the fault cluster shift size.
    const auto windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    if (windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium)
    {
        vmSettings.ComputeTopology.Memory.BackingPageSize = hcs::MemoryBackingPageSize::Small;
        vmSettings.ComputeTopology.Memory.FaultClusterSizeShift = 4;          // 64k
        vmSettings.ComputeTopology.Memory.DirectMapFaultClusterSizeShift = 4; // 64k
        m_coldDiscardShiftSize = 5;                                           // 128k
    }
    else
    {
        m_coldDiscardShiftSize = 9; // 2MB
    }

    // Configure the number of processors.
    vmSettings.ComputeTopology.Processor.Count = m_settings.CpuCount;

    // Set the vmmem suffix which will change the process name in task manager.
    if (helpers::IsVmemmSuffixSupported())
    {
        vmSettings.ComputeTopology.Memory.HostingProcessNameSuffix = m_settings.DisplayName;
    }

#ifdef _AMD64_

    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    vmSettings.ComputeTopology.Processor.EnablePerfmonPmu = hardwareFeatures.ChildPerfmonPmuSupported != 0;
    vmSettings.ComputeTopology.Processor.EnablePerfmonLbr = hardwareFeatures.ChildPerfmonLbrSupported != 0;

#endif

    // Initialize kernel command line.
    std::wstring kernelCmdLine = L"initrd=\\" LXSS_VM_MODE_INITRD_NAME L" " TEXT(WSLA_ROOT_INIT_ENV) L"=1 panic=-1";

    // Set number of processors.
    kernelCmdLine += std::format(L" nr_cpus={}", m_settings.CpuCount);

    // Enable timesync workaround to sync on resume from sleep in modern standby.
    kernelCmdLine += L" hv_utils.timesync_implicit=1";

    wil::unique_handle dmesgOutput;
    dmesgOutput = std::move(m_settings.DmesgHandle);

    m_dmesgCollector = DmesgCollector::Create(
        m_vmId, m_vmExitEvent, true, false, L"", FeatureEnabled(WslaFeatureFlagsEarlyBootDmesg), std::move(dmesgOutput));

    if (FeatureEnabled(WslaFeatureFlagsEarlyBootDmesg))
    {
        kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
    }

    // The primary "console" will be a virtio serial device.
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

    // Initialize the boot VHDs.
    std::variant<ULONG, std::string> rootVhd;
    std::variant<ULONG, std::string> modulesVhd;
    hcs::Scsi scsiController{};
    if (!FeatureEnabled(WslaFeatureFlagsPmemVhds))
    {
        ULONG nextLun = 0;
        auto attachScsiDisk = [&](PCWSTR path) {
            auto lun = nextLun;
            nextLun += 1;
            hcs::Attachment disk{};
            disk.Type = hcs::AttachmentType::VirtualDisk;
            disk.Path = path;
            disk.ReadOnly = true;
            disk.SupportCompressedVolumes = true;
            disk.AlwaysAllowSparseFiles = true;
            disk.SupportEncryptedFiles = true;
            scsiController.Attachments[std::to_string(lun)] = std::move(disk);
            AttachedDisk attachedDisk{path};
            m_attachedDisks.emplace(lun, std::move(attachedDisk));
            return lun;
        };

        rootVhd = attachScsiDisk(m_settings.RootVhd.c_str());
        modulesVhd = attachScsiDisk(kernelModulesPath.c_str());
    }
    else
    {
        hcs::VirtualPMemController pmemController{};
        pmemController.Backing = hcs::VirtualPMemBackingType::Virtual;
        ULONG nextDeviceId = 0;
        auto attachPmemDisk = [&](PCWSTR path) {
            auto deviceId = nextDeviceId;
            nextDeviceId += 1;
            hcs::VirtualPMemDevice vhd{};
            vhd.HostPath = path;
            vhd.ReadOnly = true;
            vhd.ImageFormat = hcs::VirtualPMemImageFormat::Vhd1;
            pmemController.Devices[std::to_string(deviceId)] = std::move(vhd);
            return std::format("/dev/pmem{}", deviceId);
        };

        rootVhd = attachPmemDisk(m_settings.RootVhd.c_str());
        modulesVhd = attachPmemDisk(kernelModulesPath.c_str());
        vmSettings.Devices.VirtualPMem = std::move(pmemController);
    }

    // Initialize the SCSI controller.
    vmSettings.Devices.Scsi["0"] = std::move(scsiController);

    // Construct a security descriptor that allows system and the current user.
    wil::unique_hlocal_string userSidString;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(m_userSid, &userSidString));

    std::wstring securityDescriptor = std::format(L"D:P(A;;FA;;;SY)(A;;FA;;;{})", userSidString.get());
    hcs::HvSocket hvSocketConfig{};
    hvSocketConfig.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocketConfig.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    vmSettings.Devices.HvSocket = std::move(hvSocketConfig);

    // Enable .vmrs dump collection if supported.
    if (wsl::windows::common::helpers::IsWindows11OrAbove())
    {
        CreateVmSavedStateFile();
        WI_ASSERT(!m_vmSavedStateFile.empty());

        hcs::DebugOptions debugOptions{};
        debugOptions.BugcheckSavedStateFileName = m_vmSavedStateFile;
        vmSettings.DebugOptions = std::move(debugOptions);
    }

    systemSettings.VirtualMachine = std::move(vmSettings);
    auto json = wsl::shared::ToJsonW(systemSettings);

    WSL_LOG("CreateWSLAVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    m_computeSystem = hcs::CreateComputeSystem(m_vmIdString.c_str(), json.c_str());

    auto runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_computeSystem.get());
    WI_ASSERT(IsEqualGUID(m_vmId, runtimeId));

    // Initialize DeviceHostProxy for virtio device support.
    if (FeatureEnabled(WslaFeatureFlagsVirtioFs) || m_settings.NetworkingMode == WSLANetworkingModeVirtioProxy)
    {
        m_guestDeviceManager = std::make_shared<GuestDeviceManager>(m_vmIdString, m_vmId);
    }

    wsl::windows::common::hcs::RegisterCallback(m_computeSystem.get(), &s_OnExit, this);

    wsl::windows::common::hcs::StartComputeSystem(m_computeSystem.get(), json.c_str());
    m_running = true;

    // Create a socket listening for crash dumps.
    auto crashDumpSocket = wsl::windows::common::hvsocket::Listen(runtimeId, LX_INIT_UTILITY_VM_CRASH_DUMP_PORT);
    THROW_LAST_ERROR_IF(!crashDumpSocket);

    m_crashDumpCollectionThread =
        std::thread{[this, socket = std::move(crashDumpSocket)]() mutable { CollectCrashDumps(std::move(socket)); }};

    // Create a socket listening for connections from mini_init.
    auto listenSocket = wsl::windows::common::hvsocket::Listen(runtimeId, LX_INIT_UTILITY_VM_INIT_PORT);
    auto socket = wsl::windows::common::hvsocket::Accept(listenSocket.get(), m_settings.BootTimeoutMs, m_vmTerminatingEvent.get());
    m_initChannel = wsl::shared::SocketChannel{std::move(socket), "mini_init", m_vmTerminatingEvent.get()};

    // Create a thread to watch for exited processes.
    auto [__, ___, childChannel] = Fork(WSLA_FORK::Thread);

    WSLA_WATCH_PROCESSES watchMessage{};
    childChannel.SendMessage(watchMessage);

    THROW_HR_IF(E_FAIL, childChannel.ReceiveMessage<RESULT_MESSAGE<uint32_t>>().Result != 0);
    m_processExitThread = std::thread(std::bind(&WSLAVirtualMachine::WatchForExitedProcesses, this, std::move(childChannel)));

    ConfigureNetworking();

    // Configure mounts.
    auto getDevicePath = [&](std::variant<ULONG, std::string>& vhd) -> const std::string& {
        // If the variant holds the SCSI LUN, query the guest for the device path.
        if (std::holds_alternative<ULONG>(vhd))
        {
            const auto lun = std::get<ULONG>(vhd);
            auto it = m_attachedDisks.find(lun);
            WI_ASSERT(it != m_attachedDisks.end() && it->second.Device.empty());

            it->second.Device = GetVhdDevicePath(lun);
            vhd = it->second.Device;
        }

        return std::get<std::string>(vhd);
    };

    Mount(m_initChannel, getDevicePath(rootVhd).c_str(), "/mnt", m_settings.RootVhdType.c_str(), "ro", WSLAMountFlagsChroot | WSLAMountFlagsWriteableOverlayFs);
    Mount(m_initChannel, nullptr, "/dev", "devtmpfs", "", 0);
    Mount(m_initChannel, nullptr, "/sys", "sysfs", "", 0);
    Mount(m_initChannel, nullptr, "/proc", "proc", "", 0);
    Mount(m_initChannel, nullptr, "/dev/pts", "devpts", "noatime,nosuid,noexec,gid=5,mode=620", 0);
    Mount(m_initChannel, getDevicePath(modulesVhd).c_str(), "", "ext4", "ro", WSLA_MOUNT::KernelModules);
    Mount(m_initChannel, nullptr, "/sys/fs/cgroup", "cgroup2", "", 0);

    // Configure GPU if requested.
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

        wsl::windows::common::hcs::ModifyComputeSystem(m_computeSystem.get(), wsl::shared::ToJsonW(gpuRequest).c_str());
        MountGpuLibraries("/usr/lib/wsl/lib", "/usr/lib/wsl/drivers");
    }
}

bool WSLAVirtualMachine::FeatureEnabled(WSLAFeatureFlags Value) const
{
    return static_cast<ULONG>(m_settings.FeatureFlags) & static_cast<ULONG>(Value);
}

const wil::unique_event& WSLAVirtualMachine::TerminatingEvent()
{
    return m_vmTerminatingEvent;
}

void WSLAVirtualMachine::WatchForExitedProcesses(wsl::shared::SocketChannel& Channel)
try
{
    // TODO: Terminate the VM if this thread exits unexpectedly.
    while (true)
    {
        auto [message, _] = Channel.ReceiveMessageOrClosed<WSLA_PROCESS_EXITED>();
        if (message == nullptr)
        {
            break; // Channel has been closed, exit
        }

        WSL_LOG(
            "ProcessExited",
            TraceLoggingValue(message->Pid, "Pid"),
            TraceLoggingValue(message->Code, "Code"),
            TraceLoggingValue(message->Signaled, "Signaled"));

        // Signal the exited process, if it's been monitored.
        {
            std::lock_guard lock{m_trackedProcessesLock};

            bool found = false;
            for (auto& e : m_trackedProcesses)
            {
                if (e->GetPid() == message->Pid)
                {
                    WI_ASSERT(!found);

                    try
                    {
                        e->OnExited(message->Signaled ? 128 + message->Code : message->Code);
                    }
                    CATCH_LOG();

                    found = true;
                }
            }
        }
    }
}
CATCH_LOG();

void WSLAVirtualMachine::ConfigureNetworking()
{
    switch (m_settings.NetworkingMode)
    {
    case WSLANetworkingModeNone:
        return;
    case WSLANetworkingModeNAT:
    case WSLANetworkingModeVirtioProxy:
        break;
    default:
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %lu", m_settings.NetworkingMode);
    }

    // Launch GNS
    std::vector<WSLAProcessFd> fds(1);
    fds[0].Fd = -1;
    fds[0].Type = WSLAFdType::WSLAFdTypeDefault;

    std::vector<const char*> cmd{"/gns", LX_INIT_GNS_SOCKET_ARG};

    // If DNS tunnelling is enabled, use an additional for its channel.
    if (FeatureEnabled(WslaFeatureFlagsDnsTunneling))
    {
        THROW_HR_IF_MSG(
            E_NOTIMPL, m_settings.NetworkingMode == WSLANetworkingModeVirtioProxy, "DNS tunneling not supported for VirtioProxy");

        fds.emplace_back(WSLAProcessFd{.Fd = -1, .Type = WSLAFdType::WSLAFdTypeDefault});
        THROW_IF_FAILED(wsl::core::networking::DnsResolver::LoadDnsResolverMethods());
    }

    WSLA_PROCESS_OPTIONS options{};

    // Because the file descriptors numbers aren't known in advance, the command line needs to be generated after the file
    // descriptors are allocated.
    std::string socketFdArg;
    std::string dnsFdArg;
    int gnsChannelFd = -1;
    int dnsChannelFd = -1;
    auto prepareCommandLine = [&](const auto& sockets) {
        gnsChannelFd = sockets[0].Fd;
        socketFdArg = std::to_string(gnsChannelFd);
        cmd.emplace_back(socketFdArg.c_str());

        if (sockets.size() > 1)
        {
            dnsChannelFd = sockets[1].Fd;
            dnsFdArg = std::to_string(dnsChannelFd);
            cmd.emplace_back(LX_INIT_GNS_DNS_SOCKET_ARG);
            cmd.emplace_back(dnsFdArg.c_str());
            cmd.emplace_back(LX_INIT_GNS_DNS_TUNNELING_IP);
            cmd.emplace_back(LX_INIT_DNS_TUNNELING_IP_ADDRESS);
        }

        options.CommandLine = {.Values = cmd.data(), .Count = static_cast<ULONG>(cmd.size())};
    };

    auto process = CreateLinuxProcessImpl("/init", options, fds, nullptr, prepareCommandLine);
    auto gnsChannel = wsl::core::GnsChannel(wil::unique_socket{(SOCKET)process->GetStdHandle(gnsChannelFd).release()});

    if (m_settings.NetworkingMode == WSLANetworkingModeNAT)
    {
        // TODO: refactor this to avoid using wsl config
        wsl::core::Config config(nullptr);

        // Disable HyperV Firewall if not supported
        if (!wsl::core::NatNetworking::IsHyperVFirewallSupported(config))
        {
            config.FirewallConfig.reset();
        }

        m_networkEngine = std::make_unique<wsl::core::NatNetworking>(
            m_computeSystem.get(),
            wsl::core::NatNetworking::CreateNetwork(config),
            std::move(gnsChannel),
            config,
            dnsChannelFd != -1 ? wil::unique_socket{(SOCKET)process->GetStdHandle(dnsChannelFd).release()} : wil::unique_socket{},
            nullptr);
    }
    else
    {
        m_networkEngine =
            std::make_unique<wsl::core::VirtioNetworking>(std::move(gnsChannel), false, nullptr, m_guestDeviceManager, m_userToken);
    }

    m_networkEngine->Initialize();

    LaunchPortRelay();
}

void CALLBACK WSLAVirtualMachine::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
try
{
    if (Event->Type == HcsEventSystemExited)
    {
        reinterpret_cast<WSLAVirtualMachine*>(Context)->OnExit(Event);
    }
    if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        reinterpret_cast<WSLAVirtualMachine*>(Context)->OnCrash(Event);
    }
}
CATCH_LOG()

void WSLAVirtualMachine::OnExit(_In_ const HCS_EVENT* Event)
{
    WSL_LOG(
        "WSLAVmExited", TraceLoggingValue(Event->EventData, "details"), TraceLoggingValue(static_cast<int>(Event->Type), "type"));

    m_vmExitEvent.SetEvent();

    const auto exitStatus = wsl::shared::FromJson<wsl::windows::common::hcs::SystemExitStatus>(Event->EventData);

    auto reason = WSLAlVirtualMachineTerminationReasonUnknown;

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
            reason = WSLAlVirtualMachineTerminationReasonUnknown;
            break;
        }
    }

    if (m_terminationCallback)
    {
        LOG_IF_FAILED(m_terminationCallback->OnTermination(reason, Event->EventData));
    }
}

void WSLAVirtualMachine::OnCrash(_In_ const HCS_EVENT* Event)
{
    WSL_LOG(
        "WSLAGuestCrash",
        TraceLoggingValue(Event->EventData, "details"),
        TraceLoggingValue(static_cast<int>(Event->Type), "type"));

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

std::pair<ULONG, std::string> WSLAVirtualMachine::AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly)
{
    ULONG Lun{};
    std::string Device;

    auto result = wil::ResultFromException([&]() {
        std::lock_guard lock{m_lock};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_running);

        AttachedDisk disk{Path};

        auto grantDiskAccess = [&]() {
            auto runAsUser = wil::impersonate_token(m_userToken.get());
            wsl::windows::common::hcs::GrantVmAccess(m_vmIdString.c_str(), Path);
            disk.AccessGranted = true;
        };

        if (!ReadOnly)
        {
            grantDiskAccess();
        }

        while (m_attachedDisks.find(Lun) != m_attachedDisks.end())
        {
            Lun++;
        }

        bool vhdAdded = false;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (vhdAdded)
            {
                wsl::windows::common::hcs::RemoveScsiDisk(m_computeSystem.get(), Lun);
            }

            if (disk.AccessGranted)
            {
                wsl::windows::common::hcs::RevokeVmAccess(m_vmIdString.c_str(), Path);
            }
        });

        auto result =
            wil::ResultFromException([&]() { wsl::windows::common::hcs::AddVhd(m_computeSystem.get(), Path, Lun, ReadOnly); });

        if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !disk.AccessGranted)
        {
            grantDiskAccess();
            wsl::windows::common::hcs::AddVhd(m_computeSystem.get(), Path, Lun, ReadOnly);
        }
        else
        {
            THROW_IF_FAILED(result);
        }

        vhdAdded = true;

        const auto devicePath = GetVhdDevicePath(Lun);
        cleanup.release();

        disk.Device = std::move(devicePath);
        Device = disk.Device;
        m_attachedDisks.emplace(Lun, std::move(disk));
    });

    WSL_LOG(
        "WSLAAttachDisk",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(Device.c_str(), "Device"),
        TraceLoggingValue(result, "Result"));

    THROW_IF_FAILED(result);

    return {Lun, Device};
}

void WSLAVirtualMachine::Unmount(_In_ const char* Path)
{
    auto [pid, _, subChannel] = Fork(WSLA_FORK::Thread);

    wsl::shared::MessageWriter<WSLA_UNMOUNT> message;
    message.WriteString(Path);

    const auto& response = subChannel.Transaction<WSLA_UNMOUNT>(message.Span());

    // TODO: Return errno to caller
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), response.Result == EINVAL);
    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLAVirtualMachine::DetachDisk(_In_ ULONG Lun)
{
    std::lock_guard lock{m_lock};

    // Find the disk
    auto it = m_attachedDisks.find(Lun);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    // Detach it from the guest
    WSLA_DETACH message;
    message.Lun = Lun;
    const auto& response = m_initChannel.Transaction(message);

    // TODO: Return errno to caller
    THROW_HR_IF(E_FAIL, response.Result != 0);

    // Remove it from the VM
    m_attachedDisks.erase(it);

    hcs::RemoveScsiDisk(m_computeSystem.get(), Lun);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(enum WSLA_FORK::ForkType Type)
{
    std::lock_guard lock{m_lock};
    return Fork(m_initChannel, Type);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(
    wsl::shared::SocketChannel& Channel, enum WSLA_FORK::ForkType Type, ULONG TtyRows, ULONG TtyColumns)
{
    uint32_t port{};
    int32_t pid{};
    int32_t ptyMaster{};
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_running);

        WSLA_FORK message;
        message.ForkType = Type;
        message.TtyColumns = static_cast<uint16_t>(TtyColumns);
        message.TtyRows = static_cast<uint16_t>(TtyRows);
        const auto& response = Channel.Transaction(message);
        port = response.Port;
        pid = response.Pid;
        ptyMaster = response.PtyMasterFd;
    }

    THROW_HR_IF_MSG(E_FAIL, pid <= 0, "fork() returned %i", pid);

    auto socket = wsl::windows::common::hvsocket::Connect(m_vmId, port, m_vmExitEvent.get(), m_settings.BootTimeoutMs);

    return std::make_tuple(pid, ptyMaster, wsl::shared::SocketChannel{std::move(socket), std::to_string(pid), m_vmTerminatingEvent.get()});
}

WSLAVirtualMachine::ConnectedSocket WSLAVirtualMachine::ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd)
{
    WSLA_ACCEPT message{};
    message.Fd = Fd;
    const auto& response = Channel.Transaction(message);

    ConnectedSocket socket;
    socket.Socket = wsl::windows::common::hvsocket::Connect(m_vmId, response.Result);

    // If the FD was unspecified, read the Linux file descriptor from the guest.
    if (Fd == -1)
    {
        socket.Fd = Channel.ReceiveMessage<RESULT_MESSAGE<int32_t>>().Result;
    }
    else
    {
        socket.Fd = Fd;
    }

    return socket;
}

std::string WSLAVirtualMachine::GetVhdDevicePath(ULONG Lun)
{
    WSLA_GET_DISK message{};
    message.Header.MessageSize = sizeof(message);
    message.Header.MessageType = WSLA_GET_DISK::Type;
    message.ScsiLun = Lun;
    const auto& response = m_initChannel.Transaction(message);
    THROW_HR_IF_MSG(E_FAIL, response.Result != 0, "Failed to get disk path, init returned: %lu", response.Result);

    return response.Buffer;
}

Microsoft::WRL::ComPtr<WSLAProcess> WSLAVirtualMachine::CreateLinuxProcess(
    _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS& Options, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
{
    // Check if this is a tty or not
    std::vector<WSLAProcessFd> fds;
    if (WI_IsFlagSet(Options.Flags, WSLAProcessFlagsTty))
    {
        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDTty, .Type = WSLAFdType::WSLAFdTypeTty});
        fds.emplace_back(WSLAProcessFd{.Fd = 0, .Type = WSLAFdType::WSLAFdTypeTtyControl});
    }
    else
    {
        if (WI_IsFlagSet(Options.Flags, WSLAProcessFlagsStdin))
        {
            fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStdin, .Type = WSLAFdType::WSLAFdTypeDefault});
        }

        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStdout, .Type = WSLAFdType::WSLAFdTypeDefault});
        fds.emplace_back(WSLAProcessFd{.Fd = WSLAFDStderr, .Type = WSLAFdType::WSLAFdTypeDefault});
    }

    return CreateLinuxProcessImpl(Executable, Options, fds, Errno, PrepareCommandLine);
}

Microsoft::WRL::ComPtr<WSLAProcess> WSLAVirtualMachine::CreateLinuxProcessImpl(
    LPCSTR Executable, const WSLA_PROCESS_OPTIONS& Options, const std::vector<WSLAProcessFd>& Fds, int* Errno, const TPrepareCommandLine& PrepareCommandLine)
{
    // N.B This check is there to prevent processes from being started before the VM is done initializing.
    // to avoid potential deadlocks, since the processExitThread is required to signal the process exit events.
    // std::thread::joinable() is const, so this can be called without acquiring the lock.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_processExitThread.joinable());

    auto setErrno = [Errno](int Error) {
        if (Errno != nullptr)
        {
            *Errno = Error;
        }
    };

    // Check if this is a tty or not
    const WSLAProcessFd* tty = nullptr;
    const WSLAProcessFd* ttyControl = nullptr;
    auto [pid, _, childChannel] = Fork(WSLA_FORK::Process);

    std::vector<WSLAVirtualMachine::ConnectedSocket> sockets;
    for (const auto& e : Fds)
    {
        if (e.Type == WSLAFdTypeTty)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, tty != nullptr, "Multiple terminal fds specified");
            tty = &e;
        }
        else if (e.Type == WSLAFdTypeTtyControl)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, ttyControl != nullptr, "Multiple terminal control fds specified");
            ttyControl = &e;
        }

        sockets.emplace_back(ConnectSocket(childChannel, static_cast<int32_t>(e.Fd)));
    }

    PrepareCommandLine(sockets);

    wsl::shared::MessageWriter<WSLA_EXEC> Message;

    Message.WriteString(Message->ExecutableIndex, Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options.CurrentDirectory ? Options.CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options.CommandLine.Values, Options.CommandLine.Count);
    Message.WriteStringArray(Message->EnvironmentIndex, Options.Environment.Values, Options.Environment.Count);

    // If this is an interactive tty, we need a relay process
    if (tty != nullptr)
    {
        auto [grandChildPid, ptyMaster, grandChildChannel] = Fork(childChannel, WSLA_FORK::Pty, Options.TtyRows, Options.TtyColumns);
        WSLA_TTY_RELAY relayMessage{};
        relayMessage.TtyMaster = ptyMaster;
        relayMessage.Socket = tty->Fd;
        relayMessage.TtyControl = ttyControl == nullptr ? -1 : ttyControl->Fd;
        childChannel.SendMessage(relayMessage);

        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }

        grandChildChannel.SendMessage<WSLA_EXEC>(Message.Span());
        result = ExpectClosedChannelOrError(grandChildChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }

        pid = grandChildPid;
    }
    else
    {
        childChannel.SendMessage<WSLA_EXEC>(Message.Span());
        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            setErrno(result);
            THROW_HR_MSG(E_FAIL, "errno: %i", result);
        }
    }

    wil::unique_socket ttyControlHandle;

    std::map<ULONG, wil::unique_handle> stdHandles;
    for (auto& [fd, handle] : sockets)
    {
        if (ttyControl != nullptr && fd == ttyControl->Fd)
        {
            ttyControlHandle = std::move(handle);
            continue;
        }

        stdHandles.emplace(fd, reinterpret_cast<HANDLE>(handle.release()));
    }

    auto io = std::make_unique<VMProcessIO>(std::move(stdHandles));
    auto control = std::make_unique<VMProcessControl>(*this, pid, std::move(ttyControlHandle));

    {
        std::lock_guard lock{m_trackedProcessesLock};
        m_trackedProcesses.emplace_back(control.get());
    }

    auto process = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

    setErrno(0);

    return process;
}

void WSLAVirtualMachine::Mount(LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    std::lock_guard lock{m_lock};

    Mount(m_initChannel, Source, Target, Type, Options, Flags);
}

void WSLAVirtualMachine::Mount(shared::SocketChannel& Channel, LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    static_assert(WSLAMountFlagsNone == WSLA_MOUNT::None);
    static_assert(WSLAMountFlagsReadOnly == WSLA_MOUNT::ReadOnly);
    static_assert(WSLAMountFlagsChroot == WSLA_MOUNT::Chroot);
    static_assert(WSLAMountFlagsWriteableOverlayFs == WSLA_MOUNT::OverlayFs);

    wsl::shared::MessageWriter<WSLA_MOUNT> message;

    auto optionalAdd = [&](auto value, unsigned int& index) {
        if (value != nullptr)
        {
            message.WriteString(index, value);
        }
    };

    optionalAdd(Source, message->SourceIndex);
    optionalAdd(Target, message->DestinationIndex);
    optionalAdd(Type, message->TypeIndex);
    optionalAdd(Options, message->OptionsIndex);
    message->Flags = Flags;

    const auto& response = Channel.Transaction<WSLA_MOUNT>(message.Span());

    WSL_LOG(
        "WSLAMount",
        TraceLoggingValue(Source == nullptr ? "<null>" : Source, "Source"),
        TraceLoggingValue(Target == nullptr ? "<null>" : Target, "Target"),
        TraceLoggingValue(Type == nullptr ? "<null>" : Type, "Type"),
        TraceLoggingValue(Options == nullptr ? "<null>" : Options, "Options"),
        TraceLoggingValue(Flags, "Flags"),
        TraceLoggingValue(response.Result, "Result"));

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

int32_t WSLAVirtualMachine::ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel)
{
    auto [response, span] = Channel.ReceiveMessageOrClosed<RESULT_MESSAGE<int32_t>>();
    if (response != nullptr)
    {
        return response->Result;
    }
    else
    {
        return 0;
    }
}

void WSLAVirtualMachine::Signal(_In_ LONG Pid, _In_ int Signal)
{
    std::lock_guard lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_running);

    WSLA_SIGNAL message;
    message.Pid = Pid;
    message.Signal = Signal;
    const auto& response = m_initChannel.Transaction(message);

    THROW_HR_IF(E_FAIL, response.Result != 0);
}

void WSLAVirtualMachine::RegisterCallback(ITerminationCallback* callback)
{
    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_INVALIDARG, m_terminationCallback);

    // N.B. this calls AddRef() on the callback
    m_terminationCallback = callback;
}

void WSLAVirtualMachine::LaunchPortRelay()
{
    WI_ASSERT(!m_portRelayChannelRead);

    auto [_, __, channel] = Fork(WSLA_FORK::ForkType::Process);

    std::lock_guard lock(m_portRelaylock);
    auto relayPort = channel.Transaction<WSLA_PORT_RELAY>();

    wil::unique_handle readPipe;
    wil::unique_handle writePipe;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &m_portRelayChannelWrite, nullptr, 0));
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&m_portRelayChannelRead, &writePipe, nullptr, 0));

    wsl::windows::common::helpers::SetHandleInheritable(readPipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(writePipe.get());
    wsl::windows::common::helpers::SetHandleInheritable(m_vmExitEvent.get());

    // Create a restricted token.
    auto restrictedToken = wsl::windows::common::security::CreateRestrictedToken(m_userToken.get());

    auto path = wsl::windows::common::wslutil::GetBasePath() / L"wslrelay.exe";

    auto cmd = std::format(
        L"\"{}\" {} {} {} {} {} {} {} {}",
        path,
        wslrelay::mode_option,
        static_cast<int>(wslrelay::RelayMode::WSLAPortRelay),
        wslrelay::exit_event_option,
        HandleToUlong(m_vmExitEvent.get()),
        wslrelay::port_option,
        relayPort.Result,
        wslrelay::vm_id_option,
        m_vmId);

    WSL_LOG("LaunchWslRelay", TraceLoggingValue(cmd.c_str(), "cmd"));

    wsl::windows::common::SubProcess process{nullptr, cmd.c_str()};
    process.SetStdHandles(readPipe.get(), writePipe.get(), nullptr);
    process.SetToken(restrictedToken.get());
    process.Start();

    readPipe.release();
    writePipe.release();
}

void WSLAVirtualMachine::MapPortImpl(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ bool Remove)
{
    std::lock_guard lock(m_portRelaylock);

    THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_portRelayChannelWrite);

    WSLA_MAP_PORT message;
    message.WindowsPort = WindowsPort;
    message.LinuxPort = LinuxPort;
    message.AddressFamily = Family;
    message.Stop = Remove;

    DWORD bytesTransfered{};
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(m_portRelayChannelWrite.get(), &message, sizeof(message), &bytesTransfered, nullptr));
    THROW_HR_IF_MSG(E_UNEXPECTED, bytesTransfered != sizeof(message), "%u bytes transfered", bytesTransfered);

    HRESULT result = E_UNEXPECTED;
    THROW_IF_WIN32_BOOL_FALSE(ReadFile(m_portRelayChannelRead.get(), &result, sizeof(result), &bytesTransfered, nullptr));

    THROW_HR_IF(E_UNEXPECTED, bytesTransfered != sizeof(result));
    THROW_IF_FAILED_MSG(result, "Failed to map port: WindowsPort=%d, LinuxPort=%d, Family=%d, Remove=%d", WindowsPort, LinuxPort, Family, Remove);
}

void WSLAVirtualMachine::MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    MapPortImpl(Family, WindowsPort, LinuxPort, false);
}

void WSLAVirtualMachine::UnmapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    MapPortImpl(Family, WindowsPort, LinuxPort, true);
}

HRESULT WSLAVirtualMachine::MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)
{
    return MountWindowsFolderImpl(WindowsPath, LinuxPath, ReadOnly ? WSLAMountFlagsReadOnly : WSLAMountFlagsNone);
}

HRESULT WSLAVirtualMachine::MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ WSLAMountFlags Flags)
try
{
    std::filesystem::path path(WindowsPath);
    THROW_HR_IF_MSG(E_INVALIDARG, !path.is_absolute(), "Path is not absolute: '%ls'", WindowsPath);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), !std::filesystem::is_directory(path), "Path is not a directory: '%ls'", WindowsPath);

    GUID shareGuid{};
    THROW_IF_FAILED(CoCreateGuid(&shareGuid));

    auto shareName = shared::string::GuidToString<wchar_t>(shareGuid, shared::string::None);

    std::optional<GUID> instanceId;
    {
        // Create the share on the host.
        std::lock_guard lock(m_lock);

        // Verify that this folder isn't already mounted.
        auto it = m_mountedWindowsFolders.find(LinuxPath);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), it != m_mountedWindowsFolders.end());

        if (!FeatureEnabled(WslaFeatureFlagsVirtioFs))
        {
            auto flags = hcs::Plan9ShareFlags::AllowOptions;
            WI_SetFlagIf(flags, hcs::Plan9ShareFlags::ReadOnly, WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly));
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
            instanceId = m_guestDeviceManager->AddGuestDevice(
                VIRTIO_FS_DEVICE_ID,
                m_virtioFsClassId,
                shareName.c_str(),
                L"",
                WindowsPath,
                VIRTIO_FS_FLAGS_TYPE_FILES,
                m_userToken.get());
        }

        m_mountedWindowsFolders.emplace(LinuxPath, MountedFolderInfo{shareName, instanceId});
    }

    auto deleteOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        std::lock_guard lock(m_lock);
        auto mountIt = m_mountedWindowsFolders.find(LinuxPath);
        if (WI_VERIFY(mountIt != m_mountedWindowsFolders.end()))
        {
            auto mountInfo = mountIt->second;
            m_mountedWindowsFolders.erase(mountIt);
            RemoveShare(mountInfo);
        }
    });

    // Create the guest mount
    auto shareNameUtf8 = shared::string::WideToMultiByte(shareName);
    if (!FeatureEnabled(WslaFeatureFlagsVirtioFs))
    {
        auto [_, __, channel] = Fork(WSLA_FORK::Thread);

        WSLA_CONNECT message;
        message.HostPort = LX_INIT_UTILITY_VM_PLAN9_PORT;

        auto fd = channel.Transaction(message).Result;
        THROW_HR_IF_MSG(E_FAIL, fd < 0, "WSLA_CONNECT failed with %i", fd);

        auto mountOptions = std::format(
            "{},msize={},trans=fd,rfdno={},wfdno={},aname={},cache=mmap",
            WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly) ? "ro" : "rw",
            LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE,
            fd,
            fd,
            shareNameUtf8);

        Mount(channel, shareNameUtf8.c_str(), LinuxPath, "9p", mountOptions.c_str(), Flags);
    }
    else
    {
        std::string options = WI_IsFlagSet(Flags, WSLAMountFlagsReadOnly) ? "ro" : "rw";
        Mount(m_initChannel, shareNameUtf8.c_str(), LinuxPath, "virtiofs", options.c_str(), Flags);
    }

    deleteOnFailure.release();

    return S_OK;
}
CATCH_RETURN();

void WSLAVirtualMachine::RemoveShare(_In_ const MountedFolderInfo& MountInfo)
{
    if (!FeatureEnabled(WslaFeatureFlagsVirtioFs))
    {
        WI_ASSERT(!MountInfo.InstanceId.has_value());
        hcs::RemovePlan9Share(m_computeSystem.get(), MountInfo.ShareName.c_str(), LX_INIT_UTILITY_VM_PLAN9_PORT);
    }
    else if (WI_VERIFY(MountInfo.InstanceId.has_value()))
    {
        m_guestDeviceManager->RemoveGuestDevice(VIRTIO_FS_DEVICE_ID, MountInfo.InstanceId.value());
    }
}

HRESULT WSLAVirtualMachine::UnmountWindowsFolder(_In_ LPCSTR LinuxPath)
{
    std::lock_guard lock(m_lock);

    // Verify that this folder is mounted.
    auto it = m_mountedWindowsFolders.find(LinuxPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_mountedWindowsFolders.end());

    // Unmount the folder from the guest. If the mount is not found, this most likely means that the guest unmounted it.
    auto result = wil::ResultFromException([&]() { Unmount(LinuxPath); });
    THROW_HR_IF(result, FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

    auto mountInfo = it->second;
    m_mountedWindowsFolders.erase(it);

    // Remove the share from the host
    RemoveShare(mountInfo);

    return S_OK;
}

void WSLAVirtualMachine::MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint)
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_CONFIG_VALUE), !FeatureEnabled(WslaFeatureFlagsGPU));

    auto [channel, _, __] = Fork(WSLA_FORK::Thread);

    auto windowsPath = wil::GetWindowsDirectoryW<std::wstring>();

    // Mount drivers.
    THROW_IF_FAILED(MountWindowsFolderImpl(
        std::format(L"{}\\System32\\DriverStore\\FileRepository", windowsPath).c_str(), DriversMountpoint, WSLAMountFlagsReadOnly));

    // Mount the inbox libraries.
    auto inboxLibPath = std::format(L"{}\\System32\\lxss\\lib", windowsPath);
    std::optional<std::string> inboxLibMountPoint;
    if (std::filesystem::is_directory(inboxLibPath))
    {
        inboxLibMountPoint = std::format("{}/inbox", LibrariesMountPoint);
        THROW_IF_FAILED(MountWindowsFolderImpl(inboxLibPath.c_str(), inboxLibMountPoint->c_str(), WSLAMountFlagsReadOnly));
    }

    // Mount the packaged libraries.

#ifdef WSL_GPU_LIB_PATH

    auto packagedLibPath = std::filesystem::path(TEXT(WSL_GPU_LIB_PATH));

#else

    auto packagedLibPath = wslutil::GetBasePath() / L"lib";

#endif

    auto packagedLibMountPoint = std::format("{}/packaged", LibrariesMountPoint);
    THROW_IF_FAILED(MountWindowsFolderImpl(packagedLibPath.c_str(), packagedLibMountPoint.c_str(), WSLAMountFlagsReadOnly));

    // Mount an overlay containing both inbox and packaged libraries (the packaged mount takes precedence).
    std::string options = "lowerdir=" + packagedLibMountPoint;
    if (inboxLibMountPoint.has_value())
    {
        options += ":" + inboxLibMountPoint.value();
    }

    Mount(m_initChannel, "none", LibrariesMountPoint, "overlay", options.c_str(), 0);
}

std::filesystem::path WSLAVirtualMachine::GetCrashDumpFolder()
{
    auto tempPath = wsl::windows::common::filesystem::GetTempFolderPath(m_userToken.get());
    return tempPath / L"wsla-crashes";
}

void WSLAVirtualMachine::CreateVmSavedStateFile()
{
    auto runAsUser = wil::impersonate_token(m_userToken.get());

    const auto filename = std::format(L"{}{}-{}{}", SAVED_STATE_FILE_PREFIX, std::time(nullptr), m_vmIdString, SAVED_STATE_FILE_EXTENSION);

    auto savedStateFile = m_crashDumpFolder / filename;

    wsl::windows::common::filesystem::EnsureDirectory(m_crashDumpFolder.c_str());

    wil::unique_handle file{CreateFileW(savedStateFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    THROW_LAST_ERROR_IF(!file);

    hcs::GrantVmAccess(m_vmIdString.c_str(), savedStateFile.c_str());
    m_vmSavedStateFile = savedStateFile;
}

void wsl::windows::service::wsla::WSLAVirtualMachine::EnforceVmSavedStateFileLimit()
{
    auto pred = [](const auto& e) {
        return WI_IsFlagSet(GetFileAttributes(e.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && e.path().has_extension() &&
               e.path().extension() == SAVED_STATE_FILE_EXTENSION && e.path().has_filename() &&
               e.path().filename().wstring().find(SAVED_STATE_FILE_PREFIX) == 0 && e.file_size() > 0;
    };

    wsl::windows::common::wslutil::EnforceFileLimit(m_crashDumpFolder.c_str(), MAX_VM_CRASH_FILES + 1, pred);
}

void WSLAVirtualMachine::WriteCrashLog(const std::wstring& crashLog)
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

void WSLAVirtualMachine::OnProcessReleased(int Pid)
{
    std::lock_guard lock{m_trackedProcessesLock};

    auto erased = std::erase_if(m_trackedProcesses, [Pid](const auto* e) { return e->GetPid() == Pid; });
}

void WSLAVirtualMachine::CollectCrashDumps(wil::unique_socket&& listenSocket) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"CrashDumpCollection");

    while (!m_vmExitEvent.is_signaled())
    {
        try
        {
            auto socket = wsl::windows::common::hvsocket::Accept(listenSocket.get(), INFINITE, m_vmExitEvent.get());

            THROW_LAST_ERROR_IF(
                setsockopt(listenSocket.get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&RECEIVE_TIMEOUT, sizeof(RECEIVE_TIMEOUT)) == SOCKET_ERROR);

            auto channel = wsl::shared::SocketChannel{std::move(socket), "crash_dump", m_vmExitEvent.get()};

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
        CATCH_LOG();
    }
}

// TODO: Handle reservations per family.
bool WSLAVirtualMachine::TryAllocatePort(uint16_t Port)
{
    std::lock_guard lock{m_lock};

    WSL_LOG("AllocatePort", TraceLoggingValue(Port, "Port"));

    auto [_, inserted] = m_allocatedPorts.insert(Port);

    return inserted;
}

std::set<uint16_t> WSLAVirtualMachine::AllocatePorts(uint16_t Count)
{
    std::lock_guard lock{m_lock};

    std::set<uint16_t> allocatedRange;

    // Add ports to the allocated list until we have enough
    for (auto i = CONTAINER_PORT_RANGE.first; i <= CONTAINER_PORT_RANGE.second && allocatedRange.size() < Count; i++)
    {
        if (!m_allocatedPorts.contains(i))
        {
            WI_VERIFY(allocatedRange.insert(i).second);
        }
    }

    // Fail if we couldn't find enough free ports.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES),
        allocatedRange.size() < Count,
        "Failed to allocate %u ports, only %zu available",
        Count,
        allocatedRange.size());

    // Reserve the ports we found.
    m_allocatedPorts.insert(allocatedRange.begin(), allocatedRange.end());

    return allocatedRange;
}

void WSLAVirtualMachine::ReleasePorts(const std::set<uint16_t>& Ports)
{
    std::lock_guard lock{m_lock};

    for (const auto& port : Ports)
    {
        WSL_LOG("ReleasePort", TraceLoggingValue(port, "Port"));

        WI_VERIFY(m_allocatedPorts.erase(port) == 1);
    }
}

wil::unique_socket WSLAVirtualMachine::ConnectUnixSocket(const char* Path)
{
    auto [_, __, channel] = Fork(WSLA_FORK::Thread);

    shared::MessageWriter<WSLA_UNIX_CONNECT> message;
    message.WriteString(message->PathOffset, Path);

    auto result = channel.Transaction<WSLA_UNIX_CONNECT>(message.Span());

    THROW_HR_IF_MSG(E_FAIL, result.Result < 0, "Failed to connect to unix socket: '%hs', %i", Path, result.Result);

    return channel.Release();
}