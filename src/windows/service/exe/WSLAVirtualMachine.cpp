/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVirtualMachine.cpp

Abstract:

    Class for the WSLA virtual machine.

--*/

#include "WSLAVirtualMachine.h"
#include "hcs_schema.h"
#include "NatNetworking.h"
#include "MirroredNetworking.h"
#include "WSLAUserSession.h"

using namespace wsl::windows::common;
using helpers::WindowsBuildNumbers;
using helpers::WindowsVersion;
using wsl::windows::service::wsla::WSLAVirtualMachine;

WSLAVirtualMachine::WSLAVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID UserSid, WSLAUserSessionImpl* Session) :
    m_settings(Settings), m_userSid(UserSid), m_userSession(Session)
{
    THROW_IF_FAILED(CoCreateGuid(&m_vmId));

    if (Settings.EnableDebugShell)
    {
        m_debugShellPipe = wsl::windows::common::wslutil::GetDebugShellPipeName(m_userSid) + m_settings.DisplayName;
    }
}

HRESULT WSLAVirtualMachine::GetDebugShellPipe(LPWSTR* pipePath)
{
    RETURN_HR_IF(E_INVALIDARG, m_debugShellPipe.empty());

    *pipePath = wil::make_unique_string<wil::unique_cotaskmem_string>(m_debugShellPipe.c_str()).release();

    return S_OK;
}

void WSLAVirtualMachine::OnSessionTerminating()
{
    m_userSession = nullptr;
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
    {
        std::lock_guard mutex(m_lock);

        if (m_userSession != nullptr)
        {
            m_userSession->OnVmTerminated(this);
        }
    }

    WSL_LOG("WSLATerminateVmStart", TraceLoggingValue(m_running, "running"));

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

    m_computeSystem.reset();

    for (const auto& e : m_attachedDisks)
    {
        try
        {
            wsl::windows::common::hcs::RevokeVmAccess(m_vmIdString.c_str(), e.second.Path.c_str());
        }
        CATCH_LOG()
    }
}

void WSLAVirtualMachine::Start()
{
    hcs::ComputeSystem systemSettings{};
    systemSettings.Owner = L"WSL";
    systemSettings.ShouldTerminateOnLastHandleClosed = true;
    systemSettings.SchemaVersion.Major = 2;
    systemSettings.SchemaVersion.Minor = 3;
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
    //
    // N.B. This is only done on builds that have the fix for the VID deadlock on partition teardown.
    if ((m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Germanium) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Cobalt && m_windowsVersion.UpdateBuildRevision >= 2360) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Iron && m_windowsVersion.UpdateBuildRevision >= 1970) ||
        (m_windowsVersion.BuildNumber >= WindowsBuildNumbers::Vibranium_22H2 && m_windowsVersion.UpdateBuildRevision >= 3393))
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
    if (m_settings.DmesgOutput != 0)
    {
        dmesgOutput.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(m_settings.DmesgOutput)));
    }

    m_dmesgCollector = DmesgCollector::Create(m_vmId, m_vmExitEvent, true, false, L"", true, std::move(dmesgOutput));

    if (m_settings.EnableEarlyBootDmesg)
    {
        kernelCmdLine += L" earlycon=uart8250,io,0x3f8,115200";
        vmSettings.Devices.ComPorts["0"] = hcs::ComPort{m_dmesgCollector->EarlyConsoleName()};
    }

    if (helpers::IsVirtioSerialConsoleSupported())
    {
        vmSettings.Devices.VirtioSerial.emplace();

        // The primary "console" will be a virtio serial device.

        kernelCmdLine += L" console=hvc0 debug";
        hcs::VirtioSerialPort virtioPort{};
        virtioPort.Name = L"hvc0";
        virtioPort.NamedPipe = m_dmesgCollector->VirtioConsoleName();
        virtioPort.ConsoleSupport = true;
        vmSettings.Devices.VirtioSerial->Ports["0"] = std::move(virtioPort);

        if (!m_debugShellPipe.empty())
        {
            hcs::VirtioSerialPort virtioPort;
            virtioPort.Name = L"hvc1";
            virtioPort.NamedPipe = m_debugShellPipe;
            virtioPort.ConsoleSupport = true;
            vmSettings.Devices.VirtioSerial->Ports["1"] = std::move(virtioPort);
        }
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
        // TODO
        THROW_HR(E_NOTIMPL);
        auto bootThis = hcs::UefiBootEntry{};
        bootThis.DeviceType = hcs::UefiBootDevice::VmbFs;
        // bootThis.VmbFsRootPath = m_rootFsPath.c_str();
        bootThis.DevicePath = L"\\" LXSS_VM_MODE_KERNEL_NAME;
        bootThis.OptionalData = kernelCmdLine;
        hcs::Uefi uefiSettings{};
        uefiSettings.BootThis = std::move(bootThis);
        vmSettings.Chipset.Uefi = std::move(uefiSettings);
    }

    // Initialize other devices.
    vmSettings.Devices.Scsi["0"] = hcs::Scsi{};
    hcs::HvSocket hvSocketConfig{};

    // Construct a security descriptor that allows system and the current user.
    wil::unique_hlocal_string userSidString;
    THROW_LAST_ERROR_IF(!ConvertSidToStringSidW(m_userSid, &userSidString));

    std::wstring securityDescriptor{L"D:P(A;;FA;;;SY)(A;;FA;;;"};
    securityDescriptor += userSidString.get();
    securityDescriptor += L")";
    hvSocketConfig.HvSocketConfig.DefaultBindSecurityDescriptor = securityDescriptor;
    hvSocketConfig.HvSocketConfig.DefaultConnectSecurityDescriptor = securityDescriptor;
    vmSettings.Devices.HvSocket = std::move(hvSocketConfig);

    systemSettings.VirtualMachine = std::move(vmSettings);
    auto json = wsl::shared::ToJsonW(systemSettings);

    WSL_LOG("CreateWSLAVirtualMachine", TraceLoggingValue(json.c_str(), "json"));

    m_vmIdString = wsl::shared::string::GuidToString<wchar_t>(m_vmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    m_computeSystem = hcs::CreateComputeSystem(m_vmIdString.c_str(), json.c_str());

    auto runtimeId = wsl::windows::common::hcs::GetRuntimeId(m_computeSystem.get());
    WI_ASSERT(IsEqualGUID(m_vmId, runtimeId));

    wsl::windows::common::hcs::RegisterCallback(m_computeSystem.get(), &s_OnExit, this);

    wsl::windows::common::hcs::StartComputeSystem(m_computeSystem.get(), json.c_str());

    // Create a socket listening for connections from mini_init.
    auto listenSocket = wsl::windows::common::hvsocket::Listen(runtimeId, LX_INIT_UTILITY_VM_INIT_PORT);
    auto socket = wsl::windows::common::hvsocket::Accept(listenSocket.get(), m_settings.BootTimeoutMs, m_vmTerminatingEvent.get());
    m_initChannel = wsl::shared::SocketChannel{std::move(socket), "mini_init", m_vmTerminatingEvent.get()};

    ConfigureNetworking();

    // Mount the kernel modules VHD.

#ifdef WSL_KERNEL_MODULES_PATH

    auto kernelModulesPath = std::filesystem::path(TEXT(WSL_KERNEL_MODULES_PATH));

#else

    auto kernelModulesPath = basePath / L"modules.vhd";

#endif

    wil::unique_cotaskmem_ansistring device;
    ULONG lun{};
    THROW_IF_FAILED(AttachDisk(kernelModulesPath.c_str(), true, &device, &lun));

    THROW_HR_IF_MSG(
        E_FAIL,
        MountImpl(m_initChannel, device.get(), "", "ext4", "ro", WSLA_MOUNT::KernelModules) != 0,
        "Failed to mount the kernel modules from: %hs",
        device.get());

    // Configure GPU if requested.
    if (m_settings.EnableGPU)
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
    }
}

void WSLAVirtualMachine::ConfigureNetworking()
{
    if (m_settings.NetworkingMode == WslNetworkingModeNone)
    {
        return;
    }
    else if (m_settings.NetworkingMode == WslNetworkingModeNAT)
    {
        // Launch GNS

        WSLA_PROCESS_FD fd{};
        fd.Fd = 3;
        fd.Type = WslFdType::WslFdTypeDefault;

        std::vector<const char*> cmd{"/gns", LX_INIT_GNS_SOCKET_ARG, "3"};
        WSLA_CREATE_PROCESS_OPTIONS options{};
        options.Executable = "/init";
        options.CommandLine = cmd.data();
        options.CommandLineCount = static_cast<ULONG>(cmd.size());

        std::vector<HANDLE> socketHandles(2);

        WSLA_CREATE_PROCESS_RESULT result{};
        auto sockets = CreateLinuxProcessImpl(&options, 1, &fd, &result);

        THROW_HR_IF(E_FAIL, result.Errno != 0);

        // TODO: refactor this to avoid using wsl config
        static wsl::core::Config config(nullptr);

        if (!wsl::core::MirroredNetworking::IsHyperVFirewallSupported(config))
        {
            config.FirewallConfig.reset();
        }

        // TODO: DNS Tunneling support
        m_networkEngine = std::make_unique<wsl::core::NatNetworking>(
            m_computeSystem.get(), wsl::core::NatNetworking::CreateNetwork(config), std::move(sockets[0]), config, wil::unique_socket{});

        m_networkEngine->Initialize();

        LaunchPortRelay();
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %lu", m_settings.NetworkingMode);
    }
}

void CALLBACK WSLAVirtualMachine::s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context)
{
    if (Event->Type == HcsEventSystemExited || Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
    {
        reinterpret_cast<WSLAVirtualMachine*>(Context)->OnExit(Event);
    }
}

void WSLAVirtualMachine::OnExit(_In_ const HCS_EVENT* Event)
{
    WSL_LOG(
        "WSLAVmExited", TraceLoggingValue(Event->EventData, "details"), TraceLoggingValue(static_cast<int>(Event->Type), "type"));

    m_vmExitEvent.SetEvent();

    std::lock_guard lock(m_lock);
    if (m_terminationCallback)
    {
        // TODO: parse json and give a better error.
        WslVirtualMachineTerminationReason reason = WslVirtualMachineTerminationReasonUnknown;
        if (Event->Type == HcsEventSystemExited)
        {
            reason = WslVirtualMachineTerminationReasonShutdown;
        }
        else if (Event->Type == HcsEventSystemCrashInitiated || Event->Type == HcsEventSystemCrashReport)
        {
            reason = WslVirtualMachineTerminationReasonCrashed;
        }

        LOG_IF_FAILED(m_terminationCallback->OnTermination(static_cast<ULONG>(reason), Event->EventData));
    }
}

HRESULT WSLAVirtualMachine::AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly, _Out_ LPSTR* Device, _Out_ ULONG* Lun)
try
{
    *Device = nullptr;
    auto result = wil::ResultFromException([&]() {
        std::lock_guard lock{m_lock};
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

        {
            const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
            auto runAsUser = wil::impersonate_token(userToken.get());
            wsl::windows::common::hcs::GrantVmAccess(m_vmIdString.c_str(), Path);
        }

        *Lun = 0;
        while (m_attachedDisks.find(*Lun) != m_attachedDisks.end())
        {
            (*Lun)++;
        }

        bool vhdAdded = false;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (vhdAdded)
            {
                wsl::windows::common::hcs::RemoveScsiDisk(m_computeSystem.get(), *Lun);
            }

            wsl::windows::common::hcs::RevokeVmAccess(m_vmIdString.c_str(), Path);
        });

        wsl::windows::common::hcs::AddVhd(m_computeSystem.get(), Path, *Lun, ReadOnly);
        vhdAdded = true;

        WSLA_GET_DISK message{};
        message.Header.MessageSize = sizeof(message);
        message.Header.MessageType = WSLA_GET_DISK::Type;
        message.ScsiLun = *Lun;
        const auto& response = m_initChannel.Transaction(message);

        THROW_HR_IF_MSG(E_FAIL, response.Result != 0, "Failed to attach disk, init returned: %lu", response.Result);

        cleanup.release();
        m_attachedDisks.emplace(*Lun, AttachedDisk{Path, response.Buffer});

        *Device = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(response.Buffer).release();
    });

    WSL_LOG(
        "WSLAAttachDisk",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(ReadOnly, "ReadOnly"),
        TraceLoggingValue(*Device == nullptr ? "<null>" : *Device, "Device"),
        TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags)
try
{
    THROW_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~(WslMountFlagsChroot | WslMountFlagsWriteableOverlayFs)));

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    THROW_HR_IF(E_FAIL, MountImpl(m_initChannel, Source, Target, Type, Options, Flags) != 0);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::Unmount(_In_ const char* Path)
try
{
    auto [pid, _, subChannel] = Fork(WSLA_FORK::Thread);

    wsl::shared::MessageWriter<WSLA_UNMOUNT> message;
    message.WriteString(Path);

    const auto& response = subChannel.Transaction<WSLA_UNMOUNT>(message.Span());

    // TODO: Return errno to caller
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), response.Result == EINVAL);
    THROW_HR_IF(E_FAIL, response.Result != 0);

    return S_OK;
}
CATCH_RETURN()

HRESULT WSLAVirtualMachine::DetachDisk(_In_ ULONG Lun)
try
{
    std::lock_guard lock{m_lock};

    // Find the disk
    auto it = m_attachedDisks.find(Lun);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_attachedDisks.end());

    // Detach it from the guest
    WSLA_DETACH message;
    message.Lun = Lun;
    const auto& response = m_initChannel.Transaction(message);

    // TODO: Return errno to caller
    THROW_HR_IF(E_FAIL, response.Result != 0);

    // Remove it from the VM
    m_attachedDisks.erase(it);

    hcs::RemoveScsiDisk(m_computeSystem.get(), Lun);

    return S_OK;
}
CATCH_RETURN()

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(enum WSLA_FORK::ForkType Type)
{
    std::lock_guard lock{m_lock};
    return Fork(m_initChannel, Type);
}

std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> WSLAVirtualMachine::Fork(wsl::shared::SocketChannel& Channel, enum WSLA_FORK::ForkType Type)
{
    uint32_t port{};
    int32_t pid{};
    int32_t ptyMaster{};
    {
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

        WSLA_FORK message;
        message.ForkType = Type;
        message.TtyColumns = 80;
        message.TtyRows = 80;
        const auto& response = Channel.Transaction(message);
        port = response.Port;
        pid = response.Pid;
        ptyMaster = response.PtyMasterFd;
    }

    THROW_HR_IF_MSG(E_FAIL, pid <= 0, "fork() returned %i", pid);

    auto socket = wsl::windows::common::hvsocket::Connect(m_vmId, port, m_vmExitEvent.get(), m_settings.BootTimeoutMs);

    return std::make_tuple(pid, ptyMaster, wsl::shared::SocketChannel{std::move(socket), std::to_string(pid), m_vmTerminatingEvent.get()});
}

wil::unique_socket WSLAVirtualMachine::ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd)
{
    WSLA_ACCEPT message{};
    message.Fd = Fd;
    const auto& response = Channel.Transaction(message);

    return wsl::windows::common::hvsocket::Connect(m_vmId, response.Result);
}

void WSLAVirtualMachine::OpenLinuxFile(wsl::shared::SocketChannel& Channel, const char* Path, uint32_t Flags, int32_t Fd)
{
    static_assert(WslFdTypeLinuxFileInput == WslaOpenFlagsRead);
    static_assert(WslFdTypeLinuxFileOutput == WslaOpenFlagsWrite);
    static_assert(WslFdTypeLinuxFileAppend == WslaOpenFlagsAppend);
    static_assert(WslFdTypeLinuxFileCreate == WslaOpenFlagsCreate);

    shared::MessageWriter<WSLA_OPEN> message;
    message->Fd = Fd;
    message->Flags = Flags;
    message.WriteString(Path);

    auto result = Channel.Transaction<WSLA_OPEN>(message.Span()).Result;

    THROW_HR_IF_MSG(E_FAIL, result != 0, "Failed to open %hs (flags: %u), %i", Path, Flags, result);
}

HRESULT WSLAVirtualMachine::CreateLinuxProcess(
    _In_ const WSLA_CREATE_PROCESS_OPTIONS* Options, ULONG FdCount, WSLA_PROCESS_FD* Fds, _Out_ ULONG* Handles, _Out_ WSLA_CREATE_PROCESS_RESULT* Result)
try
{
    auto sockets = CreateLinuxProcessImpl(Options, FdCount, Fds, Result);

    for (size_t i = 0; i < sockets.size(); i++)
    {
        if (sockets[i])
        {
            Handles[i] =
                HandleToUlong(wsl::windows::common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(sockets[i].get())));
        }
    }

    return S_OK;
}
CATCH_RETURN();

std::vector<wil::unique_socket> WSLAVirtualMachine::CreateLinuxProcessImpl(
    _In_ const WSLA_CREATE_PROCESS_OPTIONS* Options, _In_ ULONG FdCount, _In_ WSLA_PROCESS_FD* Fds, _Out_ WSLA_CREATE_PROCESS_RESULT* Result)
{
    // Check if this is a tty or not
    const WSLA_PROCESS_FD* ttyInput = nullptr;
    const WSLA_PROCESS_FD* ttyOutput = nullptr;
    auto interactiveTty = ParseTtyInformation(Fds, FdCount, &ttyInput, &ttyOutput);
    auto [pid, _, childChannel] = Fork(WSLA_FORK::Process);

    std::vector<wil::unique_socket> sockets(FdCount);
    for (size_t i = 0; i < FdCount; i++)
    {
        if (Fds[i].Type == WslFdTypeDefault || Fds[i].Type == WslFdTypeTerminalInput || Fds[i].Type == WslFdTypeTerminalOutput)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, Fds[i].Type > WslFdTypeTerminalOutput, "Invalid flags: %i", Fds[i].Type);
            THROW_HR_IF_MSG(E_INVALIDARG, Fds[i].Path != nullptr, "Fd[%zu] has a non-null path but flags: %i", i, Fds[i].Type);
            sockets[i] = ConnectSocket(childChannel, static_cast<int32_t>(Fds[i].Fd));
        }
        else
        {
            THROW_HR_IF_MSG(
                E_INVALIDARG,
                WI_IsAnyFlagSet(Fds[i].Type, WslFdTypeTerminalInput | WslFdTypeTerminalOutput),
                "Invalid flags: %i",
                Fds[i].Type);

            THROW_HR_IF_MSG(E_INVALIDARG, Fds[i].Path == nullptr, "Fd[%zu] has a null path but flags: %i", i, Fds[i].Type);
            OpenLinuxFile(childChannel, Fds[i].Path, Fds[i].Type, Fds[i].Fd);
        }
    }

    wsl::shared::MessageWriter<WSLA_EXEC> Message;

    Message.WriteString(Message->ExecutableIndex, Options->Executable);
    Message.WriteString(Message->CurrentDirectoryIndex, Options->CurrentDirectory ? Options->CurrentDirectory : "/");
    Message.WriteStringArray(Message->CommandLineIndex, Options->CommandLine, Options->CommandLineCount);
    Message.WriteStringArray(Message->EnvironmentIndex, Options->Environment, Options->EnvironmentCount);

    // If this is an interactive tty, we need a relay process
    if (interactiveTty)
    {
        auto [grandChildPid, ptyMaster, grandChildChannel] = Fork(childChannel, WSLA_FORK::Pty);
        WSLA_TTY_RELAY relayMessage;
        relayMessage.TtyMaster = ptyMaster;
        relayMessage.TtyInput = ttyInput->Fd;
        relayMessage.TtyOutput = ttyOutput->Fd;
        childChannel.SendMessage(relayMessage);

        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            Result->Errno = result;
            THROW_HR(E_FAIL);
        }

        grandChildChannel.SendMessage<WSLA_EXEC>(Message.Span());
        result = ExpectClosedChannelOrError(grandChildChannel);
        if (result != 0)
        {
            Result->Errno = result;
            THROW_HR(E_FAIL);
        }

        pid = grandChildPid;
    }
    else
    {
        childChannel.SendMessage<WSLA_EXEC>(Message.Span());
        auto result = ExpectClosedChannelOrError(childChannel);
        if (result != 0)
        {
            Result->Errno = result;
            THROW_HR(E_FAIL);
        }
    }

    Result->Errno = 0;
    Result->Pid = pid;
    return sockets;
}

int32_t WSLAVirtualMachine::MountImpl(shared::SocketChannel& Channel, LPCSTR Source, LPCSTR Target, LPCSTR Type, LPCSTR Options, ULONG Flags)
{
    static_assert(WslMountFlagsNone == WSLA_MOUNT::None);
    static_assert(WslMountFlagsChroot == WSLA_MOUNT::Chroot);
    static_assert(WslMountFlagsWriteableOverlayFs == WSLA_MOUNT::OverlayFs);

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

    return response.Result;
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

HRESULT WSLAVirtualMachine::WaitPid(LONG Pid, ULONGLONG TimeoutMs, ULONG* State, int* Code)
try
{
    auto [pid, _, subChannel] = Fork(WSLA_FORK::Thread);

    WSLA_WAITPID message{};
    message.Pid = Pid;
    message.TimeoutMs = TimeoutMs;

    const auto& response = subChannel.Transaction(message);

    THROW_HR_IF(E_FAIL, response.State == WSLAOpenFlagsUnknown);

    *State = response.State;
    *Code = response.Code;

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::Shutdown(ULONGLONG TimeoutMs)
try
{
    std::lock_guard lock(m_lock);

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    WSLA_SHUTDOWN message{};
    m_initChannel.SendMessage(message);
    auto response = m_initChannel.ReceiveMessageOrClosed<MESSAGE_HEADER>(static_cast<wsl::shared::TTimeout>(TimeoutMs));

    RETURN_HR_IF(E_UNEXPECTED, response.first != nullptr);

    m_running = false;
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::Signal(_In_ LONG Pid, _In_ int Signal)
try
{
    std::lock_guard lock(m_lock);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_running);

    WSLA_SIGNAL message;
    message.Pid = Pid;
    message.Signal = Signal;
    const auto& response = m_initChannel.Transaction(message);

    RETURN_HR_IF(E_FAIL, response.Result != 0);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::RegisterCallback(ITerminationCallback* callback)
try
{
    std::lock_guard lock(m_lock);

    THROW_HR_IF(E_INVALIDARG, m_terminationCallback);

    // N.B. this calls AddRef() on the callback
    m_terminationCallback = callback;

    return S_OK;
}
CATCH_RETURN();

bool WSLAVirtualMachine::ParseTtyInformation(const WSLA_PROCESS_FD* Fds, ULONG FdCount, const WSLA_PROCESS_FD** TtyInput, const WSLA_PROCESS_FD** TtyOutput)
{
    bool foundNonTtyFd = false;

    for (ULONG i = 0; i < FdCount; i++)
    {
        if (Fds[i].Type == WslFdTypeTerminalInput)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, *TtyInput != nullptr, "Only one TtyInput fd can be passed. Index=%lu", i);

            *TtyInput = &Fds[i];
        }
        else if (Fds[i].Type == WslFdTypeTerminalOutput)
        {
            THROW_HR_IF_MSG(E_INVALIDARG, *TtyOutput != nullptr, "Only one TtyOutput fd can be passed. Index=%lu", i);
            *TtyOutput = &Fds[i];
        }
        else
        {
            foundNonTtyFd = true;
        }
    }

    THROW_HR_IF_MSG(
        E_INVALIDARG, foundNonTtyFd && (*TtyOutput != nullptr || *TtyInput != nullptr), "Found mixed tty & non tty fds");

    return !foundNonTtyFd && FdCount > 0;
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

    // Get an impersonation token
    auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    auto restrictedToken = wsl::windows::common::security::CreateRestrictedToken(userToken.get());

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

HRESULT WSLAVirtualMachine::MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ BOOL Remove)
try
{
    std::lock_guard lock(m_portRelaylock);

    RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, !m_portRelayChannelWrite);

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

    return result;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)
{
    return MountWindowsFolderImpl(WindowsPath, LinuxPath, ReadOnly, WslMountFlagsNone);
}

HRESULT WSLAVirtualMachine::MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly, _In_ WslMountFlags Flags)
try
{
    std::filesystem::path Path(WindowsPath);
    THROW_HR_IF_MSG(E_INVALIDARG, !Path.is_absolute(), "Path is not absolute: '%ls'", WindowsPath);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), !std::filesystem::is_directory(Path), "Path is not a directory: '%ls'", WindowsPath);

    GUID shareGuid{};
    THROW_IF_FAILED(CoCreateGuid(&shareGuid));

    auto shareName = shared::string::GuidToString<wchar_t>(shareGuid, shared::string::None);

    {
        // Create the plan9 share on the host
        std::lock_guard lock(m_lock);

        // Verify that this folder isn't already mounted.
        auto it = m_plan9Mounts.find(LinuxPath);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), it != m_plan9Mounts.end());

        hcs::AddPlan9Share(
            m_computeSystem.get(),
            shareName.c_str(),
            shareName.c_str(),
            WindowsPath,
            LX_INIT_UTILITY_VM_PLAN9_PORT,
            hcs::Plan9ShareFlags::AllowOptions | (ReadOnly ? hcs::Plan9ShareFlags::ReadOnly : hcs::Plan9ShareFlags::None),
            wsl::windows::common::security::GetUserToken(TokenImpersonation).get());

        m_plan9Mounts.emplace(LinuxPath, shareName);
    }

    auto deleteOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        std::lock_guard lock(m_lock);

        LOG_HR_IF(E_UNEXPECTED, m_plan9Mounts.erase(LinuxPath) != 1);
    });

    // Create the guest mount
    auto [_, __, channel] = Fork(WSLA_FORK::Thread);

    WSLA_CONNECT message;
    message.HostPort = LX_INIT_UTILITY_VM_PLAN9_PORT;

    auto fd = channel.Transaction(message).Result;
    THROW_HR_IF_MSG(E_FAIL, fd < 0, "WSLA_CONNECT failed with %i", fd);

    auto shareNameUtf8 = shared::string::WideToMultiByte(shareName);
    auto mountOptions =
        std::format("msize={},trans=fd,rfdno={},wfdno={},aname={},cache=mmap", LX_INIT_UTILITY_VM_PLAN9_BUFFER_SIZE, fd, fd, shareNameUtf8);

    THROW_HR_IF(E_FAIL, MountImpl(channel, shareNameUtf8.c_str(), LinuxPath, "9p", mountOptions.c_str(), Flags) != 0);

    deleteOnFailure.release();
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::UnmountWindowsFolder(_In_ LPCSTR LinuxPath)
try
{
    std::lock_guard lock(m_lock);

    // Verify that this folder is mounted.
    auto it = m_plan9Mounts.find(LinuxPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), it == m_plan9Mounts.end());

    // Unmount the folder from the guest. If the mount is not found, this most likely means that the guest unmounted it.
    auto result = Unmount(LinuxPath);
    THROW_HR_IF(result, FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

    // Remove the share from the host
    hcs::RemovePlan9Share(m_computeSystem.get(), it->second.c_str(), LX_INIT_UTILITY_VM_PLAN9_PORT);

    m_plan9Mounts.erase(it);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAVirtualMachine::MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint, _In_ DWORD Flags)
try
{
    RETURN_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WslMountFlagsWriteableOverlayFs), "Unexpected flags: %lu", Flags);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_CONFIG_VALUE), !m_settings.EnableGPU);

    auto [channel, _, __] = Fork(WSLA_FORK::Thread);

    auto windowsPath = wil::GetWindowsDirectoryW<std::wstring>();

    // Mount drivers.
    RETURN_IF_FAILED(MountWindowsFolderImpl(
        std::format(L"{}\\System32\\DriverStore\\FileRepository", windowsPath).c_str(), DriversMountpoint, true, static_cast<WslMountFlags>(Flags)));

    // Mount the inbox libraries.
    auto inboxLibPath = std::format(L"{}\\System32\\lxss\\lib", windowsPath);
    std::optional<std::string> inboxLibMountPoint;
    if (std::filesystem::is_directory(inboxLibPath))
    {
        inboxLibMountPoint = std::format("{}/inbox", LibrariesMountPoint);
        RETURN_IF_FAILED(MountWindowsFolder(inboxLibPath.c_str(), inboxLibMountPoint->c_str(), true));
    }

    // Mount the packaged libraries.

#ifdef WSL_GPU_LIB_PATH

    auto packagedLibPath = std::filesystem::path(TEXT(WSL_GPU_LIB_PATH));

#else

    auto packagedLibPath = wslutil::GetBasePath() / L"lib";

#endif

    auto packagedLibMountPoint = std::format("{}/packaged", LibrariesMountPoint);
    RETURN_IF_FAILED(MountWindowsFolder(packagedLibPath.c_str(), packagedLibMountPoint.c_str(), true));

    // Mount an overlay containing both inbox and packaged libraries (the packaged mount takes precedence).
    std::string options = "lowerdir=" + packagedLibMountPoint;
    if (inboxLibMountPoint.has_value())
    {
        options += ":" + inboxLibMountPoint.value();
    }

    RETURN_IF_FAILED(Mount("none", LibrariesMountPoint, "overlay", options.c_str(), Flags));
    return S_OK;
}
CATCH_RETURN();